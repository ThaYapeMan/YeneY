// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_lms_cli.h"
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
namespace queue_audio {
// Queue-only CLI transport. Never called by the audio/core thread. Mutations
// are sent once; an uncertain acknowledgement is reconciled by reads only.
class LmsClient {
public:
    using Clock = std::function<uint64_t()>;
    struct Options {
        unsigned port = 9090, queryMs = 6000, mutationMs = 30000, snapshotMs = 30000;
        uint64_t outageMs = 30000;
        Clock clock = [] {
            return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
        };
        std::function<void(const std::string &)> log, unavailable;
        std::function<void(uint64_t, uint64_t)> metrics;
    };

private:
    std::string host, mac;
    Options options;
    mutable std::mutex mutex;
    std::condition_variable changed;
    struct Job {
        std::vector<std::string> commands;
        std::string reason;
        std::function<void()> done;
        uint64_t ticket;
    };
    std::deque<Job> jobs;
    std::atomic_bool closing{false}, busy{false};
    std::atomic<uint64_t> generation{1};
    uint64_t slow = 0, failed = 0, outage = 0, lastSuccess = 0, fallbackTicket = 0;
    bool notified = false;
    std::map<std::string, uint64_t> lastLog;
    void observe(const std::string &command, bool ok, uint64_t start, uint64_t duration) {
        uint64_t s, f;
        bool print = false;
        {
            std::lock_guard<std::mutex> l(mutex);
            if (duration > 1000) {
                ++slow;
                auto p = lastLog.find(command);
                print = p == lastLog.end() || options.clock() - p->second >= 60000;
                if (print) {
                    if (lastLog.size() > 128)
                        lastLog.clear();
                    lastLog[command] = options.clock();
                }
            }
            if (ok) {
                lastSuccess = options.clock();
                outage = 0;
                notified = false;
            } else {
                ++failed;
                if (!outage)
                    outage = std::max(start, lastSuccess);
            }
            s = slow;
            f = failed;
        }
        if (print && options.log)
            options.log("queue LMS slow command=" + command + " duration_ms=" + std::to_string(duration));
        if (options.metrics)
            options.metrics(s, f);
    }
    void notify() {
        bool send = false;
        {
            std::lock_guard<std::mutex> l(mutex);
            if (outage && options.clock() - outage >= options.outageMs && !notified) {
                notified = true;
                send = true;
            }
        }
        if (send && options.unavailable)
            options.unavailable("lms-unavailable");
    }
    bool exchange(const std::string &command, std::string &wire, unsigned timeout,
                  const std::function<bool(const std::string &)> &valid = {}) {
        auto at = options.clock();
        auto start = std::chrono::steady_clock::now();
        bool ok = controlLms(host, mac, command, options.port, &wire, timeout, &closing);
        if (ok && valid) {
            try {
                ok = valid(wire);
            } catch (...) {
                ok = false;
            }
        }
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                .count();
        observe(command, ok, at, uint64_t(ms));
        notify();
        return ok;
    }
    static std::pair<std::string, std::string> verification(const std::string &c) {
        if (c == "play")
            return {"mode ?", "play"};
        if (c == "pause 1")
            return {"mode ?", "pause"};
        if (c == "stop")
            return {"mode ?", "stop"};
        for (auto prefix : {std::string("playlist index "), std::string("time ")})
            if (c.rfind(prefix, 0) == 0)
                return {prefix + "?", c.substr(prefix.size())};
        return {};
    }
    std::thread worker;
    bool apply(const std::string &command, uint64_t ticket) {
        if (closing || generation != ticket)
            return false;
        const auto started = options.clock();
        std::string wire;
        const bool acknowledged = exchange(command, wire, options.mutationMs);
        auto check = verification(command);
        if (check.first.empty() && acknowledged)
            return true;
        bool contradicted = false;
        // No mutation retry, including after a dropped connection. Let a slow
        // in-flight LMS operation finish before interpreting the old state.
        while (!closing && generation == ticket) {
            if (!check.first.empty() && query(check.first, wire)) {
                const auto at = wire.find_last_of(' ');
                auto got = decodeLms(wire.substr(at + 1));
                if (!got.empty() && got.back() == '\r')
                    got.pop_back();
                bool same = got == check.second;
                if (command.rfind("time ", 0) == 0) {
                    try {
                        const auto delta = std::stod(got) - std::stod(check.second);
                        // A playing track can advance while LMS sends a slow
                        // acknowledgement; that progress is not a contradiction.
                        same = delta >= -2 && delta <= double(options.clock() - started) / 1000 + 2;
                    } catch (...) {
                        same = false;
                    }
                }
                if (same)
                    return true;
                contradicted = true;
            }
            notify();
            if (options.clock() - started >= options.outageMs && contradicted) {
                if (options.unavailable)
                    options.unavailable("lms-unavailable");
                return false;
            }
            std::unique_lock<std::mutex> l(mutex);
            changed.wait_for(l, std::chrono::milliseconds(250), [&] { return closing.load(); });
        }
        return false;
    }
    void run() {
        while (!closing) {
            Job job;
            {
                std::unique_lock<std::mutex> l(mutex);
                changed.wait_for(l, std::chrono::milliseconds(100), [&] { return closing || !jobs.empty(); });
                if (closing)
                    break;
                if (jobs.empty()) {
                    l.unlock();
                    notify();
                    continue;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            busy = true;
            bool ok = true;
            const auto ticket = job.ticket;
            for (const auto &c : job.commands) {
                if (options.log)
                    options.log("queue LMS mutation reason=" + job.reason + " command=" + c);
                if (generation != ticket || !apply(c, ticket)) {
                    ok = false;
                    break;
                }
            }
            if (ok && !closing && generation == ticket && job.done)
                job.done();
            busy = false;
        }
    }

public:
    LmsClient(std::string h, std::string m, Options o)
        : host(std::move(h)), mac(std::move(m)), options(std::move(o)) {
        worker = std::thread([this] { run(); });
    }
    ~LmsClient() { shutdown(); }
    void shutdown() {
        closing = true;
        changed.notify_all();
        if (worker.joinable())
            worker.join();
    }
    bool query(const std::string &command, std::string &wire,
               const std::function<bool(const std::string &)> &valid = {}) {
        if (command.rfind("status ", 0) != 0 &&
            (command.size() < 2 || command.compare(command.size() - 2, 2, " ?") != 0))
            return false; // read-only API cannot bypass the mutation choke point
        for (int i = 0; i < 2 && !closing; ++i)
            if (exchange(command, wire,
                         command.rfind("status ", 0) == 0 ? options.snapshotMs : options.queryMs, valid))
                return true;
            else {
                std::lock_guard<std::mutex> l(mutex);
                if (outage && options.clock() - outage >= options.outageMs)
                    break;
            }
        return false;
    }
    bool submit(std::vector<std::string> commands, const std::string &reason,
                std::function<void()> done = {}) {
        if ((reason != "sonos-user-action" && reason != "fallback") || commands.empty())
            return false;
        for (const auto &command : commands)
            if (verification(command).first.empty())
                return false;
        std::lock_guard<std::mutex> l(mutex);
        if (closing || jobs.size() >= 256)
            return false;
        if (reason == "fallback") {
            if (fallbackTicket == generation.load())
                return false;
            fallbackTicket = generation.load();
        }
        jobs.push_back({std::move(commands), reason, std::move(done), generation.load()});
        changed.notify_one();
        return true;
    }
    void cancelPending() {
        std::lock_guard<std::mutex> l(mutex);
        ++generation;
        jobs.clear();
        changed.notify_all();
    }
    bool pending() const {
        std::lock_guard<std::mutex> l(mutex);
        return busy || !jobs.empty();
    }
    void tick() { notify(); }
    std::pair<uint64_t, uint64_t> counts() const {
        std::lock_guard<std::mutex> l(mutex);
        return {slow, failed};
    }
};
} // namespace queue_audio
