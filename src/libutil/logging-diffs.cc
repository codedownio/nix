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

void addFields(json & json, const Logger::Fields & fields)
{
    if (fields.empty()) return;
    auto & arr = json["fields"] = json::array();
    for (auto & f : fields)
        if (f.type == Logger::Field::tInt)
            arr.push_back(f.i);
        else if (f.type == Logger::Field::tString)
            arr.push_back(f.s);
        else
            abort();
}

void to_json(json & j, const NixMessage & m) {
    j = json{ {"level", m.level} };

    if (m.line.has_value()) j["line"] = m.line.value();
    if (m.column.has_value()) j["column"] = m.column.value();
    if (m.file.has_value()) j["file"] = m.file.value();

    if (m.trace.has_value()) j["trace"] = m.trace.value();

    if (!m.msg.empty()) j["msg"] = m.msg;
    if (!m.raw_msg.empty()) j["raw_msg"] = m.raw_msg;
}

void to_json(json & j, const ActivityState & as) {
    j = json{ {"is_complete", as.is_complete}, {"type", as.type}, {"text", as.text} };
    addFields(j, as.fields);
}

void to_json(json & j, const NixBuildState & s) {
    j = json{ {"messages", s.messages} };

    j["activities"] = json(json::value_t::object);
    for (const auto& [key, value] : s.activities) {
        j["activities"][std::to_string(key)] = value;
    }
}

void add_pos_to_message(NixMessage & msg, std::shared_ptr<AbstractPos> pos)
{
    if (pos) {
        msg.line = pos->line;
        msg.column = pos->column;
        std::ostringstream str;
        pos->print(str);
        msg.file = str.str();
    } else {
        msg.line = std::nullopt;
        msg.column = std::nullopt;
        msg.file = std::nullopt;
    }
}

void pos_to_json(json & json, std::shared_ptr<AbstractPos> pos)
{
    if (pos) {
        json["line"] = pos->line;
        json["column"] = pos->column;
        std::ostringstream str;
        pos->print(str);
        json["file"] = str.str();
    } else {
        json["line"] = nullptr;
        json["column"] = nullptr;
        json["file"] = nullptr;
    }
}

struct DiffLogger : Logger {
    Logger & prevLogger;

    NixBuildState state;
    json last_sent;
    std::mutex lock;
    std::atomic_bool exitPeriodicAction;
    std::atomic_bool exited;
    std::thread printerThread;

    DiffLogger(Logger & prevLogger) : prevLogger(prevLogger),
                                      last_sent(nullptr),
                                      exitPeriodicAction(false),
                                      exited(false),
                                      printerThread(std::thread(&DiffLogger::periodicAction, this)) { }

    // Note: tried to move the contents of the stop() fn to ~DiffLogger, but couldn't get
    // it to run.

    ~DiffLogger() {
        this->stop();
    }

    void stop() {
        // Make stop() idempotent
        if (this->exitPeriodicAction) return;

        this->exitPeriodicAction = true;
        this->printerThread.join();
        sendLatestIfNecessary();
        this->exited = true;
    }

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
        this->sendLatestIfNecessaryUnlocked();
    }

    void sendLatestIfNecessaryUnlocked() {
        if (this->last_sent == this->state) return;

        write(json::diff(this->last_sent, this->state));
        this->last_sent = this->state;
    }

    bool isVerbose() override {
        return true;
    }

    void write(const json & json)
    {
        prevLogger.log(lvlError, json.dump(-1, ' ', false, json::error_handler_t::replace));
    }

    void log(Verbosity lvl, const FormatOrString & fs) override
    {
        std::lock_guard<std::mutex> guard(lock);
        NixMessage msg;
        msg.msg = fs.s;
        this->state.messages.push_back(msg);

        // Not sure why, but sometimes log messages happen after stop() is called
        if (this->exited) sendLatestIfNecessaryUnlocked();
    }

    void logEI(const ErrorInfo & ei) override
    {
        NixMessage msg;

        std::ostringstream oss;
        showErrorInfo(oss, ei, loggerSettings.showTrace.get());

        msg.level = ei.level;
        msg.msg = oss.str();
        msg.raw_msg = ei.msg.str();

        add_pos_to_message(msg, ei.errPos);

        if (loggerSettings.showTrace.get() && !ei.traces.empty()) {
            json traces = json::array();
            for (auto iter = ei.traces.rbegin(); iter != ei.traces.rend(); ++iter) {
                json stackFrame;
                stackFrame["raw_msg"] = iter->hint.str();
                pos_to_json(stackFrame, iter->pos);
                traces.push_back(stackFrame);
            }

            msg.trace = traces;
        }

        std::lock_guard<std::mutex> guard(lock);
        this->state.messages.push_back(msg);

        // Not sure why, but sometimes log messages happen after stop() is called
        if (this->exited) sendLatestIfNecessaryUnlocked();
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
        catch (const std::out_of_range& oor) {
            Logger::writeToStdout("Failed to look up result of type " + type);
        }
    }
};

Logger * makeDiffLogger(Logger & prevLogger)
{
    return new DiffLogger(prevLogger);
}

}
