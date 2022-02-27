#pragma once

#include "types.hh"
#include "error.hh"
#include "config.hh"

#include <nlohmann/json.hpp>
#include <nlohmann/adl_serializer.hpp>
using json = nlohmann::json;

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

    ActivityState(ActivityType _type, const std::string _text, const Logger::Fields &_fields, ActivityId _parent): type(_type), text(_text), fields(_fields), parent(_parent) {

    }
};

void addFields(nlohmann::json & json, const Logger::Fields & fields)
{
    if (fields.empty()) return;
    auto & arr = json["fields"] = nlohmann::json::array();
    for (auto & f : fields)
        if (f.type == Logger::Field::tInt)
            arr.push_back(f.i);
        else if (f.type == Logger::Field::tString)
            arr.push_back(f.s);
        else
            abort();
}

void to_json(nlohmann::json & j, const ActivityState & as) {
    j = nlohmann::json{ {"is_complete", as.is_complete}, {"type", as.type}, {"text", as.text} };
    addFields(j, as.fields);
}

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

void to_json(nlohmann::json & j, const NixBuildState & s) {
    j = nlohmann::json{ {"activities", s.activities}, {"messages", s.messages} };
}

}
