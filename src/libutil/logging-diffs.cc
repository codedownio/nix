
#include "nix/util/configuration.hh"
#include "nix/util/logging.hh"
#include "nix/util/logging-diffs.hh"
#include "nix/util/position.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

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

void to_json(json & j, const NixMessage & m)
{
    j = json{ {"level", m.level} };

    if (m.line.has_value()) j["line"] = m.line.value();
    if (m.column.has_value()) j["column"] = m.column.value();
    if (m.file.has_value()) j["file"] = m.file.value();

    if (m.trace.has_value()) j["trace"] = m.trace.value();

    if (!m.msg.empty()) j["msg"] = m.msg;
    if (!m.raw_msg.empty()) j["raw_msg"] = m.raw_msg;
}

void to_json(json & j, const ActivityState & as)
{
    j = json{ {"is_complete", as.isComplete}, {"type", as.type}, {"text", as.text} };
    addFields(j, as.fields);
}

void to_json(json & j, const NixBuildState & s)
{
    j = json{ {"messages", s.messages} };

    j["activities"] = json(json::value_t::object);
    for (const auto& [key, value] : s.activities) {
        j["activities"][std::to_string(key)] = value;
    }
}

static void addPosToMessage(NixMessage & msg, std::shared_ptr<const Pos> pos)
{
    if (pos) {
        msg.line = pos->line;
        msg.column = pos->column;
        std::ostringstream str;
        pos->print(str, true);
        msg.file = str.str();
    } else {
        msg.line = std::nullopt;
        msg.column = std::nullopt;
        msg.file = std::nullopt;
    }
}

static void posToJson(json & json, std::shared_ptr<const Pos> pos)
{
    if (pos) {
        json["line"] = pos->line;
        json["column"] = pos->column;
        std::ostringstream str;
        pos->print(str, true);
        json["file"] = str.str();
    } else {
        json["line"] = nullptr;
        json["column"] = nullptr;
        json["file"] = nullptr;
    }
}

namespace {

struct DiffLogger : Logger {
    Descriptor fd;
    std::optional<std::set<ActivityType>> activity_types_to_include;

    Sync<NixBuildState> state;
    bool dirty = false; // guarded by the state lock

    // The send path (last_sent, pendingOutput, broken, the fd writes) is guarded by sendMutex,
    // which is never taken while holding the state lock, so a stalled consumer can't block the
    // logging calls.
    std::mutex sendMutex;
    json last_sent;
    std::string pendingOutput;
    bool broken = false;

    std::mutex quitMutex;
    std::condition_variable quitCV;
    bool threadDone = false; // guarded by quitMutex

    std::atomic_bool exitPeriodicAction;
    std::atomic_bool exited;
    std::thread printerThread;

    DiffLogger(Descriptor fd, std::optional<std::set<ActivityType>> activity_types_to_include)
        : fd(fd)
        , activity_types_to_include(activity_types_to_include)
        , last_sent(nullptr)
        , exitPeriodicAction(false)
        , exited(false)
    {
        // All writes go through flushPending, which handles EAGAIN with a bounded poll, so the
        // logger can never wedge the process behind a consumer that stopped draining stderr.
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        printerThread = std::thread(&DiffLogger::periodicAction, this);
    }

    // Note: tried to move the contents of the stop() fn to ~DiffLogger, but couldn't get
    // it to run.

    ~DiffLogger() {
        this->stop();
    }

    void stop() override {
        // Make stop() idempotent
        if (this->exitPeriodicAction.exchange(true)) return;

        bool done;
        {
            std::unique_lock<std::mutex> g(quitMutex);
            quitCV.notify_all();
            done = quitCV.wait_for(g, std::chrono::seconds(5), [&] { return this->threadDone; });
        }
        // The printer thread's writes are bounded, so this should be immediate; if something is
        // stuck anyway, detach rather than hang the whole process at exit. The logger is leaked
        // (never destructed), so a detached thread can't use freed memory.
        if (done) this->printerThread.join();
        else this->printerThread.detach();

        this->exited = true;
        sendLatestIfNecessary(std::chrono::milliseconds(2000));
    }

    void periodicAction() {
        try {
            // Send initial value as a normal value
            {
                std::unique_lock<std::mutex> sendLock(sendMutex);
                json current;
                {
                    auto state_(state.lock());
                    this->dirty = false;
                    current = *state_;
                }
                queueLine(current.dump(-1, ' ', false, json::error_handler_t::replace));
                this->last_sent = std::move(current);
                flushPending(std::chrono::milliseconds(100));
            }

            while (true) {
                if (this->exitPeriodicAction) break;

                sendLatestIfNecessary(std::chrono::milliseconds(100));

                std::unique_lock<std::mutex> g(quitMutex);
                quitCV.wait_for(g, std::chrono::milliseconds(300), [&] { return this->exitPeriodicAction.load(); });
            }
        } catch (...) { }

        {
            std::lock_guard<std::mutex> g(quitMutex);
            this->threadDone = true;
        }
        quitCV.notify_all();
    }

    void sendLatestIfNecessary(std::chrono::milliseconds writeTimeout) {
        std::unique_lock<std::mutex> sendLock(sendMutex);

        if (this->broken) return;

        // Finish any partially written line first; generating a new diff only once the buffer has
        // drained both keeps the stream well formed and coalesces updates while the consumer is
        // slow. dirty stays set in the meantime, so nothing is lost.
        if (!flushPending(writeTimeout)) return;

        json current;
        {
            auto state_(state.lock());
            if (!this->dirty) return;
            this->dirty = false;
            current = *state_;
        }

        if (this->last_sent == current) return;

        queueLine(json::diff(this->last_sent, current).dump(-1, ' ', false, json::error_handler_t::replace));
        this->last_sent = std::move(current);
        flushPending(writeTimeout);
    }

    bool isVerbose() override {
        return true;
    }

    void queueLine(std::string && s)
    {
        pendingOutput = std::move(s);
        pendingOutput += '\n';
    }

    // Write as much of pendingOutput as possible before the deadline. Returns true when the
    // buffer has been emptied.
    bool flushPending(std::chrono::milliseconds writeTimeout)
    {
        auto deadline = std::chrono::steady_clock::now() + writeTimeout;
        size_t written = 0;
        while (written < pendingOutput.size()) {
            ssize_t res = ::write(fd, pendingOutput.data() + written, pendingOutput.size() - written);
            if (res > 0) { written += res; continue; }
            if (res == 0) break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                poll(&pfd, 1, (int) std::min<long long>(remainingMs, 100));
                continue;
            }
            // Unrecoverable (e.g. the consumer closed the pipe): go quiet instead of crashing the
            // process or retrying forever.
            this->broken = true;
            pendingOutput.clear();
            return false;
        }
        pendingOutput.erase(0, written);
        return pendingOutput.empty();
    }

    void log(Verbosity lvl, std::string_view s) override
    {
        {
            auto state_(state.lock());
            NixMessage msg;
            msg.msg = s;
            state_->messages.push_back(msg);
            this->dirty = true;
        }

        // Not sure why, but sometimes log messages happen after stop() is called
        if (this->exited) sendLatestIfNecessary(std::chrono::milliseconds(100));
    }

    void logEI(const ErrorInfo & ei) override
    {
        NixMessage msg;

        std::ostringstream oss;
        showErrorInfo(oss, ei, loggerSettings.showTrace.get());

        msg.level = ei.level;
        msg.msg = oss.str();
        msg.raw_msg = ei.msg.str();

        addPosToMessage(msg, ei.pos);

        if (loggerSettings.showTrace.get() && !ei.traces.empty()) {
            json traces = json::array();
            for (auto iter = ei.traces.rbegin(); iter != ei.traces.rend(); ++iter) {
                json stackFrame;
                stackFrame["raw_msg"] = iter->hint.str();
                posToJson(stackFrame, iter->pos);
                traces.push_back(stackFrame);
            }

            msg.trace = traces;
        }

        {
            auto state_(state.lock());
            state_->messages.push_back(msg);
            this->dirty = true;
        }

        // Not sure why, but sometimes log messages happen after stop() is called
        if (this->exited) sendLatestIfNecessary(std::chrono::milliseconds(100));
    }

    void startActivity(ActivityId act, Verbosity lvl, ActivityType type,
        const std::string & s, const Fields & fields, ActivityId parent) override
    {
        ActivityState as(type, s, fields, parent);

        auto state_(state.lock());

        if (!activity_types_to_include || activity_types_to_include->contains(type)) {
            state_->activities.insert(std::pair<ActivityId, ActivityState>(act, as));
            this->dirty = true;
        } else {
            state_->ignored_activites.insert(act);
        }
    }

    void stopActivity(ActivityId act) override
    {
        auto state_(state.lock());

        if (activity_types_to_include && state_->ignored_activites.contains(act)) {
            state_->ignored_activites.erase(act);
        } else {
            try {
                state_->activities.at(act).isComplete = true;
                this->dirty = true;
            }
            catch (const std::out_of_range& oor) { }
        }
    }

    void result(ActivityId act, ResultType type, const Fields & fields) override
    {
        auto state_(state.lock());

        if (!activity_types_to_include || !state_->ignored_activites.contains(act)) {
            try {
                state_->activities.at(act).fields = fields;
                this->dirty = true;
            }
            catch (const std::out_of_range& oor) {
                Logger::writeToStdout("Failed to look up activity " + std::to_string(static_cast<int>(type)) + " to write result of type " + std::to_string(static_cast<int>(type)));
            }
        }
    }
};

} // namespace

std::unique_ptr<Logger> makeDiffLogger(Descriptor fd, std::optional<std::set<ActivityType>> activity_types_to_include)
{
    return std::make_unique<DiffLogger>(fd, activity_types_to_include);
}

std::unique_ptr<Logger> makeDiffLogger(Descriptor fd)
{
    return std::make_unique<DiffLogger>(fd, std::nullopt);
}

}
