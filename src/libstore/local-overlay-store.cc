#include <regex>

#include "nix/store/local-overlay-store.hh"
#include "nix/util/callback.hh"
#include "nix/store/realisation.hh"
#include "nix/util/processes.hh"
#include "nix/util/url.hh"
#include "nix/store/store-open.hh"
#include "nix/store/store-registration.hh"

namespace nix {

std::string LocalOverlayStoreConfig::doc()
{
    return
#include "local-overlay-store.md"
        ;
}

ref<Store> LocalOverlayStoreConfig::openStore() const
{
    return make_ref<LocalOverlayStore>(
        ref{std::dynamic_pointer_cast<const LocalOverlayStoreConfig>(shared_from_this())});
}

StoreReference LocalOverlayStoreConfig::getReference() const
{
    return {
        .variant =
            StoreReference::Specified{
                .scheme = *uriSchemes().begin(),
            },
    };
}

Path LocalOverlayStoreConfig::toUpperPath(const StorePath & path) const
{
    return upperLayer + "/" + path.to_string();
}

LocalOverlayStore::LocalOverlayStore(ref<const Config> config)
    : Store{*config}
    , LocalFSStore{*config}
    , LocalStore{static_cast<ref<const LocalStore::Config>>(config)}
    , config{config}
    , lowerStore(openStore(percentDecode(config->lowerStoreUri.get())).dynamic_pointer_cast<LocalFSStore>())
{
    if (config->checkMount.get()) {
        std::smatch match;
        std::string mountInfo;
        auto mounts = readFile(std::filesystem::path{"/proc/self/mounts"});
        auto regex = std::regex(R"((^|\n)overlay )" + config->realStoreDir.get() + R"( .*(\n|$))");

        // Mount points can be stacked, so there might be multiple matching entries.
        // Loop until the last match, which will be the current state of the mount point.
        while (std::regex_search(mounts, match, regex)) {
            mountInfo = match.str();
            mounts = match.suffix();
        }

        auto checkOption = [&](std::string option, std::string value) {
            return std::regex_search(mountInfo, std::regex("\\b" + option + "=" + value + "( |,)"));
        };

        auto expectedLowerDir = lowerStore->config.realStoreDir.get();
        if (!checkOption("lowerdir", expectedLowerDir) || !checkOption("upperdir", config->upperLayer)) {
            debug("expected lowerdir: %s", expectedLowerDir);
            debug("expected upperdir: %s", config->upperLayer);
            debug("actual mount: %s", mountInfo);
            throw Error("overlay filesystem '%s' mounted incorrectly", config->realStoreDir.get());
        }
    }
}

void LocalOverlayStore::registerDrvOutput(const Realisation & info)
{
    // First do queryRealisation on lower layer to populate DB
    auto res = lowerStore->queryRealisation(info.id);
    if (res)
        LocalStore::registerDrvOutput(*res);

    LocalStore::registerDrvOutput(info);
}

void LocalOverlayStore::queryPathInfoUncached(
    const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    LocalStore::queryPathInfoUncached(
        path, {[this, path, callbackPtr](std::future<std::shared_ptr<const ValidPathInfo>> fut) {
            try {
                auto info = fut.get();
                if (info)
                    return (*callbackPtr)(std::move(info));
            } catch (...) {
                return callbackPtr->rethrow();
            }
            // If we don't have it, check lower store
            lowerStore->queryPathInfo(path, {[path, callbackPtr](std::future<ref<const ValidPathInfo>> fut) {
                                          try {
                                              (*callbackPtr)(fut.get().get_ptr());
                                          } catch (...) {
                                              return callbackPtr->rethrow();
                                          }
                                      }});
        }});
}

void LocalOverlayStore::queryRealisationUncached(
    const DrvOutput & drvOutput, Callback<std::shared_ptr<const Realisation>> callback) noexcept
{
    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    LocalStore::queryRealisationUncached(
        drvOutput, {[this, drvOutput, callbackPtr](std::future<std::shared_ptr<const Realisation>> fut) {
            try {
                auto info = fut.get();
                if (info)
                    return (*callbackPtr)(std::move(info));
            } catch (...) {
                return callbackPtr->rethrow();
            }
            // If we don't have it, check lower store
            lowerStore->queryRealisation(
                drvOutput, {[callbackPtr](std::future<std::shared_ptr<const Realisation>> fut) {
                    try {
                        (*callbackPtr)(fut.get());
                    } catch (...) {
                        return callbackPtr->rethrow();
                    }
                }});
        }});
}

bool LocalOverlayStore::isValidPathUncached(const StorePath & path)
{
    // A validity check is a *read*: answer from the upper DB, falling back to the lower, WITHOUT
    // copying anything into the upper. Previously this eagerly synced the path and its entire
    // reference closure into the upper DB on every call. During a build's evaluation that means
    // copying the whole touched closure (nixpkgs & friends -- ~100s of MB of path-info) into a fresh
    // per-build upper DB, dwarfing the actual work. The upper DB only *needs* an entry for a lower
    // path when we are about to write something that references it (registration); we now do that
    // sync on demand there instead (see ensureInUpper / registerValidPaths).
    if (LocalStore::isValidPathUncached(path) || lowerStore->isValidPath(path))
        return true;
    // lazy-derivation-writes: a derivation written as a plain file (writeDerivation skips
    // registration under this setting) counts as valid, so goals/dry-run/readDerivation work
    // without any database row. Only .drv paths get this treatment; their contents are
    // content-addressed, so a present file is trustworthy.
    return settings.lazyDerivationWrites && path.isDerivation() && pathExists(toRealPath(printStorePath(path)));
}

void LocalOverlayStore::ensureInUpper(const StorePath & path)
{
    // Ensure `path` has a row in the upper DB (copying its metadata -- and, recursively, that of
    // its references -- up from the lower store), so a subsequent write that references it can look
    // it up. This is the sync that isValidPathUncached used to do eagerly; we now call it only where
    // the upper DB genuinely must have the entry (a reference of a path being registered, or a .drv
    // whose output map we register). References are synced first, so each path's own reference rows
    // resolve when it is registered. No-op if already in the upper, or not valid in the lower.
    if (LocalStore::isValidPathUncached(path))
        return;
    if (!lowerStore->isValidPath(path))
        return;
    auto p = lowerStore->queryPathInfo(path);
    for (auto & r : p->references)
        if (r != path)
            ensureInUpper(r);
    // Register directly against the base (not the virtual registerValidPath, which would re-enter
    // our override) now that the references are present.
    LocalStore::registerValidPaths({{p->path, *p}});
}

void LocalOverlayStore::queryReferrers(const StorePath & path, StorePathSet & referrers)
{
    LocalStore::queryReferrers(path, referrers);
    lowerStore->queryReferrers(path, referrers);
}

void LocalOverlayStore::queryGCReferrers(const StorePath & path, StorePathSet & referrers)
{
    LocalStore::queryReferrers(path, referrers);
}

StorePathSet LocalOverlayStore::queryValidDerivers(const StorePath & path)
{
    auto res = LocalStore::queryValidDerivers(path);
    for (const auto & p : lowerStore->queryValidDerivers(path))
        res.insert(p);
    return res;
}

std::optional<StorePath> LocalOverlayStore::queryPathFromHashPart(const std::string & hashPart)
{
    auto res = LocalStore::queryPathFromHashPart(hashPart);
    if (res)
        return res;
    else
        return lowerStore->queryPathFromHashPart(hashPart);
}

void LocalOverlayStore::registerValidPaths(const ValidPathInfos & infos)
{
    // Every reference of a path we register must have a row in the upper DB, because the base
    // registerValidPaths resolves each reference with a raw upper-DB lookup (queryValidPathId) that
    // throws "path is not valid" on a miss. Sync those references (and their closures) up from the
    // lower store now. Since isValidPathUncached no longer syncs eagerly, this on-demand sync -- of
    // just what the writes actually reference -- is what keeps registration correct.
    for (auto & [_, info] : infos)
        for (auto & r : info.references)
            if (r != info.path)
                ensureInUpper(r);

    // First, get any from lower store so we merge
    {
        StorePathSet notInUpper;
        for (auto & [p, _] : infos)
            if (!LocalStore::isValidPathUncached(p)) // avoid divergence
                notInUpper.insert(p);
        auto pathsInLower = lowerStore->queryValidPaths(notInUpper);
        ValidPathInfos inLower;
        for (auto & p : pathsInLower)
            inLower.insert_or_assign(p, *lowerStore->queryPathInfo(p));
        LocalStore::registerValidPaths(inLower);
    }
    // Then do original request
    LocalStore::registerValidPaths(infos);
}

std::map<std::string, std::optional<StorePath>>
LocalOverlayStore::queryStaticPartialDerivationOutputMap(const StorePath & path)
{
    // The base looks up the .drv's row with a raw upper-DB query (queryValidPathId), which throws
    // for a .drv that lives only in the lower store. Since isValidPathUncached no longer syncs the
    // .drv up on validity checks, read the output map from whichever layer actually has the .drv --
    // preferring the upper, else the lower -- rather than forcing a sync.
    if (LocalStore::isValidPathUncached(path))
        return LocalStore::queryStaticPartialDerivationOutputMap(path);
    return lowerStore->queryStaticPartialDerivationOutputMap(path);
}

void LocalOverlayStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    LocalStore::collectGarbage(options, results);

    remountIfNecessary();
}

void LocalOverlayStore::deleteStorePath(const Path & path, uint64_t & bytesFreed)
{
    auto mergedDir = config->realStoreDir.get() + "/";
    if (path.substr(0, mergedDir.length()) != mergedDir) {
        warn("local-overlay: unexpected gc path '%s' ", path);
        return;
    }

    StorePath storePath = {path.substr(mergedDir.length())};
    auto upperPath = config->toUpperPath(storePath);

    if (pathExists(upperPath)) {
        debug("upper exists: %s", path);
        if (lowerStore->isValidPath(storePath)) {
            debug("lower exists: %s", storePath.to_string());
            // Path also exists in lower store.
            // We must delete via upper layer to avoid creating a whiteout.
            deletePath(upperPath, bytesFreed);
            _remountRequired = true;
        } else {
            // Path does not exist in lower store.
            // So we can delete via overlayfs and not need to remount.
            LocalStore::deleteStorePath(path, bytesFreed);
        }
    }
}

void LocalOverlayStore::optimiseStore()
{
    Activity act(*logger, actOptimiseStore);

    // Note for LocalOverlayStore, queryAllValidPaths only returns paths in upper layer
    auto paths = queryAllValidPaths();

    act.progress(0, paths.size());

    uint64_t done = 0;

    for (auto & path : paths) {
        if (lowerStore->isValidPath(path)) {
            uint64_t bytesFreed = 0;
            // Deduplicate store path
            deleteStorePath(Store::toRealPath(path), bytesFreed);
        }
        done++;
        act.progress(done, paths.size());
    }

    remountIfNecessary();
}

LocalStore::VerificationResult LocalOverlayStore::verifyAllValidPaths(RepairFlag repair)
{
    StorePathSet done;

    auto existsInStoreDir = [&](const StorePath & storePath) {
        return pathExists(config->realStoreDir.get() + "/" + storePath.to_string());
    };

    bool errors = false;
    StorePathSet validPaths;

    for (auto & i : queryAllValidPaths())
        verifyPath(i, existsInStoreDir, done, validPaths, repair, errors);

    return {
        .errors = errors,
        .validPaths = validPaths,
    };
}

void LocalOverlayStore::remountIfNecessary()
{
    if (!_remountRequired)
        return;

    if (config->remountHook.get().empty()) {
        warn("'%s' needs remounting, set remount-hook to do this automatically", config->realStoreDir.get());
    } else {
        runProgram(config->remountHook, false, {config->realStoreDir});
    }

    _remountRequired = false;
}

static RegisterStoreImplementation<LocalOverlayStore::Config> regLocalOverlayStore;

} // namespace nix
