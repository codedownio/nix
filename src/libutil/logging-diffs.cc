
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

static json fieldsToJson(const Logger::Fields & fields)
{
    auto arr = json::array();
    for (auto & f : fields)
        if (f.type == Logger::Field::tInt)
            arr.push_back(f.i);
        else if (f.type == Logger::Field::tString)
            arr.push_back(f.s);
        else
            abort();
    return arr;
}

void addFields(json & json, const Logger::Fields & fields)
{
    if (fields.empty()) return;
    json["fields"] = fieldsToJson(fields);
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

    if (!as.results.empty()) {
        auto & results = j["results"] = json::object();
        for (const auto & [type, fields] : as.results)
            results[std::to_string((int) type)] = fieldsToJson(fields);
    }
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

// Builder output can outrun a slow consumer indefinitely, so the queue of unsent lines is
// capped; past the cap the oldest lines are dropped and reported as a gap in the stream.
constexpr size_t maxPendingLogBytes = 16 * 1024 * 1024;
// Bound on how many lines go into a single patch, so one flush can't produce an enormous line.
constexpr size_t maxLogLinesPerFlush = 2000;
// Rough per-entry cost of the JSON wrapper around a line, so the byte cap accounts for a flood
// of short lines.
constexpr size_t logLineOverhead = 64;

// At exit, how long to keep trying once the consumer has stopped taking bytes altogether. This
// is what bounds exit against a dead consumer; a slow one is limited by nothing but the size of
// the queue.
constexpr auto exitStallTimeout = std::chrono::milliseconds(2000);
// Backstop on the whole drain, in case results keep arriving after stop().
constexpr auto exitDeadline = std::chrono::seconds(60);

struct DiffLogger : Logger {
    Descriptor fd;
    std::optional<std::set<ActivityType>> activity_types_to_include;

    Sync<NixBuildState> state;

    // What changed since the last flush, so each flush costs O(changes) rather than
    // serializing and diffing the whole accumulated state. All guarded by the state lock.
    size_t sentMessages = 0;
    std::set<ActivityId> dirtyNew;
    std::set<ActivityId> dirtyExisting;

    // Builder output waiting to be sent. Unlike the rest of the state this is drained as it's
    // sent rather than kept, since a big build's log is far too large to hold. Guarded by the
    // state lock.
    std::deque<NixLogLine> pendingLogs;
    size_t pendingLogBytes = 0;
    uint64_t droppedLogLines = 0;

    std::atomic_bool printBuildLogs{false};

    // The send path (pendingOutput, broken, the fd writes) is guarded by sendMutex, which is
    // never taken while holding the state lock, so a stalled consumer can't block the logging
    // calls.
    std::mutex sendMutex;
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
        // Keep going while there's a backlog, since a build's last lines can exceed what one
        // flush sends. What ends this is the consumer stopping: flushPending gives up after
        // exitStallTimeout with no bytes taken at all. A consumer that keeps reading, however
        // slowly, gets the whole log — the queue is capped, so there is a bounded amount to
        // hand over. The deadline is only a backstop against results still arriving after
        // stop(), which would otherwise keep refilling the queue as fast as we drain it.
        auto deadline = std::chrono::steady_clock::now() + exitDeadline;
        while (sendLatestIfNecessary(exitStallTimeout, deadline)
               && std::chrono::steady_clock::now() < deadline) ;

        // Anything still queued isn't going to make it. Count it so the last thing the consumer
        // sees is the size of the gap, rather than a stream that just stops.
        {
            auto state_(state.lock());
            this->droppedLogLines += this->pendingLogs.size();
            this->pendingLogs.clear();
            this->pendingLogBytes = 0;
        }
        sendLatestIfNecessary(exitStallTimeout);
    }

    void periodicAction() {
        try {
            // Send initial value as a normal value
            {
                std::unique_lock<std::mutex> sendLock(sendMutex);
                json current;
                {
                    auto state_(state.lock());
                    current = *state_;
                    this->sentMessages = state_->messages.size();
                    this->dirtyNew.clear();
                    this->dirtyExisting.clear();
                }
                // Only declare the array when we're actually streaming logs, so the default
                // `diffs` output stays exactly what it was. Log lines are never part of the
                // snapshot; they only ever arrive as patches.
                if (this->printBuildLogs) current["logs"] = json::array();
                queueLine(current.dump(-1, ' ', false, json::error_handler_t::replace));
                flushPending(std::chrono::milliseconds(100));
            }

            while (true) {
                if (this->exitPeriodicAction) break;

                // A backlog means we hit the per-flush line cap, so keep going instead of
                // sleeping; otherwise a verbose build would be throttled to the tick rate.
                if (sendLatestIfNecessary(std::chrono::milliseconds(100))) continue;

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

    // Returns true if there is more to send: the caller should come back immediately rather
    // than waiting for the next tick. False on the paths where we made no progress, so a
    // stalled consumer can't turn this into a spin.
    bool sendLatestIfNecessary(std::chrono::milliseconds writeTimeout) {
        return sendLatestIfNecessary(writeTimeout, std::chrono::steady_clock::now() + writeTimeout);
    }

    bool sendLatestIfNecessary(std::chrono::milliseconds stallTimeout, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> sendLock(sendMutex);

        if (this->broken) return false;

        // Finish any partially written line first; generating a new patch only once the buffer
        // has drained both keeps the stream well formed and coalesces updates while the consumer
        // is slow. The deltas stay recorded in the meantime, so nothing is lost.
        if (!flushPending(stallTimeout, deadline)) return false;

        bool backlog = false;
        json ops = json::array();
        {
            auto state_(state.lock());

            for (auto act : this->dirtyNew) {
                auto it = state_->activities.find(act);
                if (it == state_->activities.end()) continue;
                ops.push_back(json{ {"op", "add"}, {"path", "/activities/" + std::to_string(act)}, {"value", it->second} });
            }
            this->dirtyNew.clear();

            for (auto act : this->dirtyExisting) {
                auto it = state_->activities.find(act);
                if (it == state_->activities.end()) continue;
                ops.push_back(json{ {"op", "replace"}, {"path", "/activities/" + std::to_string(act)}, {"value", it->second} });
            }
            this->dirtyExisting.clear();

            for (size_t i = this->sentMessages; i < state_->messages.size(); i++)
                ops.push_back(json{ {"op", "add"}, {"path", "/messages/-"}, {"value", state_->messages[i]} });
            this->sentMessages = state_->messages.size();

            // Everything dropped so far precedes everything still queued, so one marker here
            // sits exactly where the hole is.
            if (this->droppedLogLines) {
                ops.push_back(json{ {"op", "add"}, {"path", "/logs/-"}, {"value", json{ {"dropped", this->droppedLogLines} }} });
                this->droppedLogLines = 0;
            }

            // Consecutive lines from the same activity are sent as one entry. Repeating the op
            // wrapper and the activity id per line costs more bytes than the lines themselves.
            for (size_t sent = 0; sent < maxLogLinesPerFlush && !this->pendingLogs.empty(); ) {
                auto act = this->pendingLogs.front().activity;
                auto type = this->pendingLogs.front().type;

                json lines = json::array();
                while (sent < maxLogLinesPerFlush && !this->pendingLogs.empty()
                       && this->pendingLogs.front().activity == act
                       && this->pendingLogs.front().type == type) {
                    auto & l = this->pendingLogs.front();
                    this->pendingLogBytes -= l.line.size() + logLineOverhead;
                    lines.push_back(std::move(l.line));
                    this->pendingLogs.pop_front();
                    sent++;
                }

                ops.push_back(json{ {"op", "add"}, {"path", "/logs/-"},
                    {"value", json{ {"activity", act}, {"type", (int) type}, {"lines", lines} }} });
            }
            backlog = !this->pendingLogs.empty();
        }

        if (ops.empty()) return false;

        queueLine(ops.dump(-1, ' ', false, json::error_handler_t::replace));
        return flushPending(stallTimeout, deadline) && backlog;
    }

    // Requires the state lock to be held.
    void pushLogLine(ActivityId act, ResultType type, const Fields & fields) {
        std::string line;
        if (!fields.empty() && fields[0].type == Field::tString) line = fields[0].s;

        this->pendingLogBytes += line.size() + logLineOverhead;
        this->pendingLogs.push_back(NixLogLine{act, type, std::move(line)});

        while (this->pendingLogBytes > maxPendingLogBytes && !this->pendingLogs.empty()) {
            this->pendingLogBytes -= this->pendingLogs.front().line.size() + logLineOverhead;
            this->pendingLogs.pop_front();
            this->droppedLogLines++;
        }
    }

    // Requires the state lock to be held.
    void markActivityDirty(ActivityId act) {
        if (!this->dirtyNew.contains(act)) this->dirtyExisting.insert(act);
    }

    // When we aren't streaming builder output, say we're not verbose so that a failed build
    // still appends its last log lines to the error message.
    bool isVerbose() override {
        return this->printBuildLogs;
    }

    void setPrintBuildLogs(bool printBuildLogs) override {
        this->printBuildLogs = printBuildLogs;
    }

    void queueLine(std::string && s)
    {
        pendingOutput = std::move(s);
        pendingOutput += '\n';
    }

    bool flushPending(std::chrono::milliseconds writeTimeout)
    {
        return flushPending(writeTimeout, std::chrono::steady_clock::now() + writeTimeout);
    }

    // Write as much of pendingOutput as possible, giving up once the consumer has taken no bytes
    // at all for stallTimeout, or once deadline passes. A consumer that keeps accepting bytes is
    // never cut off for merely being slow, however long the line takes. Returns true when the
    // buffer has been emptied.
    bool flushPending(std::chrono::milliseconds stallTimeout, std::chrono::steady_clock::time_point deadline)
    {
        auto lastProgress = std::chrono::steady_clock::now();
        size_t written = 0;
        while (written < pendingOutput.size()) {
            ssize_t res = ::write(fd, pendingOutput.data() + written, pendingOutput.size() - written);
            if (res > 0) {
                written += res;
                lastProgress = std::chrono::steady_clock::now();
                continue;
            }
            if (res == 0) break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline || now - lastProgress >= stallTimeout) break;
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                auto remaining = std::min(deadline, lastProgress + stallTimeout) - now;
                auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
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
            msg.level = lvl;
            msg.msg = s;
            state_->messages.push_back(msg);
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
            this->dirtyNew.insert(act);
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
                markActivityDirty(act);
            }
            catch (const std::out_of_range& oor) { }
        }
    }

    void result(ActivityId act, ResultType type, const Fields & fields) override
    {
        if (type == resBuildLogLine || type == resPostBuildLogLine) {
            if (!this->printBuildLogs) return;

            auto state_(state.lock());
            if (activity_types_to_include && state_->ignored_activites.contains(act)) return;
            pushLogLine(act, type, fields);
            return;
        }

        auto state_(state.lock());

        if (!activity_types_to_include || !state_->ignored_activites.contains(act)) {
            try {
                // `fields` now stays as startActivity set it; results live in `results`.
                state_->activities.at(act).results[type] = fields;
                markActivityDirty(act);
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
