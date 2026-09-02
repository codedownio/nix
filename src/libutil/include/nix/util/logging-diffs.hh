#pragma once

#include "nix/util/types.hh"
#include "nix/util/error.hh"
#include "nix/util/configuration.hh"
#include "nix/util/logging.hh"

#include <nlohmann/json.hpp>

#include <deque>
#include <map>
#include <optional>
#include <set>

namespace nix {

std::unique_ptr<Logger> makeDiffLogger(Descriptor fd);
std::unique_ptr<Logger> makeDiffLogger(Descriptor fd, std::optional<std::set<ActivityType>> activity_types_to_include);

struct ActivityState {
    bool isComplete;
    ActivityType type;
    std::string text;
    Logger::Fields fields;
    ActivityId parent;

    ActivityState(ActivityType _type, const std::string _text, const Logger::Fields &_fields, ActivityId _parent):
        isComplete(false),
        type(_type),
        text(_text),
        fields(_fields),
        parent(_parent) { }
};

struct NixMessage {
    int level = 0;

    std::optional<int> line;
    std::optional<int> column;
    std::optional<std::string> file;

    std::optional<nlohmann::json> trace;

    std::string msg;
    std::string raw_msg;
};

/**
 * A line of builder output, streamed to the consumer as an entry in the top-level `logs` array.
 * The line is not attached to its activity, because activity updates are sent as whole-object
 * `replace` ops, which would wipe out an array nested under the activity.
 */
struct NixLogLine {
    ActivityId activity;
    ResultType type;
    std::string line;
};

struct NixBuildState {
    std::map<ActivityId, ActivityState> activities;
    std::set<ActivityId> ignored_activites;
    std::deque<NixMessage> messages;
};

}
