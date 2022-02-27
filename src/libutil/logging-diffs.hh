#pragma once

#include "types.hh"
#include "error.hh"
#include "config.hh"
#include "logging.hh"

#include <nlohmann/json.hpp>

#include <list>
#include <map>
#include <optional>

namespace nix {

Logger * makeJSONLogger(Logger & prevLogger);

struct ActivityState {
    bool is_complete;
    ActivityType type;
    std::string text;
    Logger::Fields fields;
    ActivityId parent;

    ActivityState(ActivityType _type, const std::string _text, const Logger::Fields &_fields, ActivityId _parent):
        is_complete(false),
        type(_type),
        text(_text),
        fields(_fields),
        parent(_parent) { }
};

struct NixMessage {
    int level;

    std::optional<int> line;
    std::optional<int> column;
    std::optional<std::string> file;

    std::optional<nlohmann::json> trace;

    std::string msg;
    std::string raw_msg;
};

struct NixBuildState {
    std::map<ActivityId, ActivityState> activities;
    std::list<NixMessage> messages;
};

}