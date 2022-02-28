#pragma once

#include "types.hh"
#include "error.hh"
#include "config.hh"
#include "logging.hh"

#include <nlohmann/json.hpp>

#include <list>
#include <map>

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
    int line;
    int column;
    std::string file;
    std::string msg;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(NixMessage, level, line, column, file, msg)
};

struct NixBuildState {
    std::map<ActivityId, ActivityState> activities;
    std::list<NixMessage> messages;
};

}
