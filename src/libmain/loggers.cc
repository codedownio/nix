#include "nix/main/loggers.hh"
#include "nix/util/logging-diffs.hh"
#include "nix/util/environment-variables.hh"
#include "nix/main/progress-bar.hh"

#include <sstream>

namespace nix {

LogFormat defaultLogFormat = LogFormat::raw;

std::optional<std::set<ActivityType>> diffActivitiesToInclude = std::nullopt;

static void parseActivityIds(const std::string & activityIdsStr)
{
    if (activityIdsStr.empty()) {
        diffActivitiesToInclude = std::nullopt;
        return;
    }

    std::set<ActivityType> activityTypes;
    std::stringstream ss(activityIdsStr);
    std::string item;

    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        item.erase(item.find_last_not_of(" \t\n\r\f\v") + 1);
        item.erase(0, item.find_first_not_of(" \t\n\r\f\v"));

        if (!item.empty()) {
            try {
                int activityTypeInt = std::stoi(item);
                activityTypes.insert(static_cast<ActivityType>(activityTypeInt));
            } catch (const std::exception& e) {
                throw Error("invalid activity type '%s' in activity IDs list", item);
            }
        }
    }

    diffActivitiesToInclude = activityTypes;
}

LogFormat parseLogFormat(const std::string & logFormatStr)
{
    if (logFormatStr == "raw" || getEnv("NIX_GET_COMPLETIONS"))
        return LogFormat::raw;
    else if (logFormatStr == "raw-with-logs")
        return LogFormat::rawWithLogs;
    else if (logFormatStr == "internal-json")
        return LogFormat::internalJSON;
    else if (logFormatStr == "diffs") {
        diffActivitiesToInclude = std::nullopt;
        return LogFormat::diffs;
    }
    else if (logFormatStr.starts_with("diffs;")) {
        std::string activityIdsPart = logFormatStr.substr(6);
        parseActivityIds(activityIdsPart);
        return LogFormat::diffs;
    }
    else if (logFormatStr == "bar")
        return LogFormat::bar;
    else if (logFormatStr == "bar-with-logs")
        return LogFormat::barWithLogs;
    throw Error("option 'log-format' has an invalid value '%s'", logFormatStr);
}

std::unique_ptr<Logger> makeDefaultLogger()
{
    switch (defaultLogFormat) {
    case LogFormat::raw:
        return makeSimpleLogger(false);
    case LogFormat::rawWithLogs:
        return makeSimpleLogger(true);
    case LogFormat::internalJSON:
        return makeJSONLogger(getStandardError());
    case LogFormat::diffs:
        return makeDiffLogger(getStandardError(), diffActivitiesToInclude);
    case LogFormat::bar:
        return makeProgressBar();
    case LogFormat::barWithLogs: {
        auto logger = makeProgressBar();
        logger->setPrintBuildLogs(true);
        return logger;
    }
    default:
        unreachable();
    }
}

void setLogFormat(const std::string & logFormatStr)
{
    setLogFormat(parseLogFormat(logFormatStr));
}

void setLogFormat(const LogFormat & logFormat)
{
    defaultLogFormat = logFormat;
    logger = makeDefaultLogger();
}

} // namespace nix
