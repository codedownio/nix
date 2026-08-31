---
synopsis: Read-only `file://` binary caches work again as substituters
issues: []
prs: []
---

Opening a local binary cache no longer fails when its directory cannot be
written to. `LocalBinaryCacheStore` creates `nar`, `build-trace-v2`, `log` and
optionally `debuginfo` up front; on a cache the caller can only read — a store
path, a read-only bind mount, a directory owned by someone else — that failed
with `Read-only file system` or `Permission denied`, and the cache was quietly
dropped from the substituter list.

This was easy to hit after the build trace directory was renamed from
`realisations` to `build-trace-v2`: caches produced by an older Nix have no
`build-trace-v2`, so the directory that had always existed suddenly had to be
created. Substitution silently fell through to the next substituter, which for
most users means everything came from `cache.nixos.org` instead of the local
cache.

Uploading to a writable cache is unaffected: `upsertFile` already creates the
directory it writes into.
