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

namespace nix {

struct DiffLogger : Logger {
    Logger & prevLogger;

    NixBuildState state;
    std::mutex lock;
    std::atomic_bool exitPeriodicAction;
    std::thread printerThread;

    DiffLogger(Logger & prevLogger) : prevLogger(prevLogger),
                                      exitPeriodicAction(false),
                                      printerThread(std::thread(&DiffLogger::periodicAction, this))
    {

    }

    void periodicAction() {
        while (true) {
            if (this->exitPeriodicAction) break;

            prevLogger.log(lvlError, "Periodic action!");

            {
                std::lock_guard<std::mutex> guard(lock);
                nlohmann::json j = this->state;
                prevLogger.log(lvlError, j.dump());
            }

            std::chrono::seconds dura(1);
            std::this_thread::sleep_for(dura);
        }
    }

    void stop() {
        prevLogger.log(lvlError, "STOP");

        std::chrono::seconds dura(5);
        std::this_thread::sleep_for(dura);

        this->exitPeriodicAction = true;
        this->printerThread.join();
    }

    bool isVerbose() override {
        return true;
    }

    void write(const nlohmann::json & json)
    {
        prevLogger.log(lvlError, "@nix " + json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    void log(Verbosity lvl, const FormatOrString & fs) override
    {
        prevLogger.log(lvlError, "log!");
    }

    void logEI(const ErrorInfo & ei) override
    {
        prevLogger.log(lvlError, "logEI!");
    }

    void startActivity(ActivityId act, Verbosity lvl, ActivityType type,
        const std::string & s, const Fields & fields, ActivityId parent) override
    {
        prevLogger.log(lvlError, "startActivity!");
        ActivityState as(type, s, fields, parent);
        std::lock_guard<std::mutex> guard(lock);
        this->state.activities.insert(std::pair<ActivityId, ActivityState>(act, as));
    }

    void stopActivity(ActivityId act) override
    {
        prevLogger.log(lvlError, "stopActivity!");
        std::lock_guard<std::mutex> guard(lock);
        try { this->state.activities.at(act).is_complete = true; }
        catch (const std::out_of_range& oor) { }
    }

    void result(ActivityId act, ResultType type, const Fields & fields) override
    {
        prevLogger.log(lvlError, "result!");
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
