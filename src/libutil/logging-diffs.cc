#include "logging.hh"
#include "logging-diffs.hh"
#include "util.hh"
#include "config.hh"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

namespace nix {

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

void to_json(nlohmann::json & j, const NixMessage & m) {
    j = nlohmann::json{ {"level", m.level} };

    if (m.line.has_value()) j["line"] = m.line.value();
    if (m.column.has_value()) j["column"] = m.column.value();
    if (m.file.has_value()) j["file"] = m.file.value();
}

void to_json(nlohmann::json & j, const ActivityState & as) {
    j = nlohmann::json{ {"is_complete", as.is_complete}, {"type", as.type}, {"text", as.text} };
    addFields(j, as.fields);
}

void to_json(nlohmann::json & j, const NixBuildState & s) {
    j = nlohmann::json{ {"messages", s.messages} };

    j["activities"] = json(json::value_t::object);
    for (const auto& [key, value] : s.activities) {
        j["activities"][std::to_string(key)] = value;
    }
}

struct DiffLogger : Logger {
    Logger & prevLogger;

    NixBuildState state;
    json last_sent;
    std::mutex lock;
    std::atomic_bool exitPeriodicAction;
    std::thread printerThread;

    DiffLogger(Logger & prevLogger) : prevLogger(prevLogger),
                                      last_sent(nullptr),
                                      exitPeriodicAction(false),
                                      printerThread(std::thread(&DiffLogger::periodicAction, this)) { }

    void periodicAction() {
        // Send initial value as a normal value
        write(this->state);
        this->last_sent = this->state;

        while (true) {
            if (this->exitPeriodicAction) break;

            sendLatestIfNecessary();

            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    void sendLatestIfNecessary() {
        std::lock_guard<std::mutex> guard(lock);

        if (this->last_sent == this->state) return;

        write(json::diff(this->last_sent, this->state));
        this->last_sent = this->state;
    }

    void stop() {
        this->exitPeriodicAction = true;
        this->printerThread.join();
        sendLatestIfNecessary();
    }

    bool isVerbose() override {
        return true;
    }

    void write(const json & json)
    {
        prevLogger.log(lvlError, "@nix " + json.dump(-1, ' ', false, json::error_handler_t::replace));
    }

    void log(Verbosity lvl, const FormatOrString & fs) override
    {
        std::lock_guard<std::mutex> guard(lock);
        NixMessage msg;
        msg.msg = fs.s;
        this->state.messages.push_back(msg);
    }

    void logEI(const ErrorInfo & ei) override
    {
        NixMessage msg;

        std::ostringstream oss;
        showErrorInfo(oss, ei, loggerSettings.showTrace.get());

        msg.level = ei.level;
        msg.msg = oss.str();
        msg.raw_msg = ei.msg.str();

        if (ei.errPos.has_value() && (*ei.errPos)) {
            msg.line = ei.errPos->line;
            msg.column = ei.errPos->column;
            msg.file = ei.errPos->file;
        }

        if (loggerSettings.showTrace.get() && !ei.traces.empty()) {
            nlohmann::json traces = nlohmann::json::array();
            for (auto iter = ei.traces.rbegin(); iter != ei.traces.rend(); ++iter) {
                nlohmann::json stackFrame;
                stackFrame["raw_msg"] = iter->hint.str();
                if (iter->pos.has_value() && (*iter->pos)) {
                    stackFrame["line"] = iter->pos->line;
                    stackFrame["column"] = iter->pos->column;
                    stackFrame["file"] = iter->pos->file;
                }
                traces.push_back(stackFrame);
            }

            msg.trace = traces;
        }

        std::lock_guard<std::mutex> guard(lock);
        this->state.messages.push_back(msg);
    }

    void startActivity(ActivityId act, Verbosity lvl, ActivityType type,
        const std::string & s, const Fields & fields, ActivityId parent) override
    {
        ActivityState as(type, s, fields, parent);
        std::lock_guard<std::mutex> guard(lock);
        this->state.activities.insert(std::pair<ActivityId, ActivityState>(act, as));
    }

    void stopActivity(ActivityId act) override
    {
        std::lock_guard<std::mutex> guard(lock);
        try { this->state.activities.at(act).is_complete = true; }
        catch (const std::out_of_range& oor) { }
    }

    void result(ActivityId act, ResultType type, const Fields & fields) override
    {
        std::lock_guard<std::mutex> guard(lock);
        try { this->state.activities.at(act).fields = fields; }
        catch (const std::out_of_range& oor) { }
    }
};

Logger * makeDiffLogger(Logger & prevLogger)
{
    return new DiffLogger(prevLogger);
}

}
