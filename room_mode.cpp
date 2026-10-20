// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "room_mode.h"
#include <atomic>
#include <stdexcept>
namespace bridge {
RoomMode::RoomMode(std::string r, std::string c, PlaybackSelection selection, Hooks h)
    : hooks(std::move(h)), room(std::move(r)), coordinator(std::move(c)) {
    s.requested = selection.requested;
    s.active = playbackModeName(selection.selected);
    s.reason = selection.reason;
    if (s.active == "queue")
        s.state = "prepare";
    log(s);
}

bool RoomMode::accepts(uint64_t g, bool queue) const {
    auto v = status();
    return g == v.generation && (queue ? v.active == "queue" && v.state != "handoff" : v.active == "stream");
}
void RoomMode::log(const ModeStatus &v) const {
    if (!hooks.log)
        return;
    std::ostringstream o;
    o << "mode room=" << room << " coordinator=" << coordinator << " requested=" << v.requested
      << " active=" << v.active << " state=" << v.state << " reason=" << v.reason
      << " occurrence=" << v.item.occurrence << " epoch=" << v.item.epoch << " generation=" << v.generation
      << " old_rate=" << v.oldRate << " new_rate=" << v.newRate << " promised=" << v.item.promised
      << " actual=" << v.item.actual << " ownership=" << v.item.owned << " ahead=" << v.item.ahead
      << " handoff_gap_ms=" << v.gapMs;
    hooks.log(o.str());
}
bool RoomMode::newPlay(bool ready, uint64_t epoch, uint64_t now) {
    ModeStatus v;
    {
        std::lock_guard<std::mutex> l(mutex);
        if (rearmBlocked || (started && !stopped))
            return false;
        if (s.requested != "queue" || !ready) {
            stopped = false;
            return false;
        }
        while (!rearms.empty() && now >= rearms.front() && now - rearms.front() > 5000)
            rearms.pop_front();
        if (now && rearms.size() >= 3) {
            rearmBlocked = true;
        } else {
            if (now)
                rearms.push_back(now);
            started = true;
        }
        if (!rearmBlocked) {
            stopped = false;
            freezeFailed = false;
            s.latched = false;
            s.active = "queue";
            s.state = "prepare";
            s.reason = "finite-ready";
            s.item = {};
            s.oldRate = s.newRate = 0;
            s.item.epoch = epoch;
            s.item.generation = ++s.generation;
            prepareAt = now;
            endAt = 0;
            s.gapMs = -1;
            v = s;
        }
    }
    if (rearmBlocked) {
        std::string recent;
        for (char c : commands)
            recent += c;
        if (hooks.log)
            hooks.log("rearm-loop room=" + room + " last_commands=" + recent);
        if (loopHandler)
            loopHandler();
        else
            request("rearm-loop", now);
        return false;
    }
    log(v);
    return true;
}
void RoomMode::command(char c) {
    std::lock_guard<std::mutex> l(mutex);
    commands.push_back(c);
    if (commands.size() > 16)
        commands.pop_front();
}
void RoomMode::adoptPlay() {
    std::lock_guard<std::mutex> l(mutex);
    stopped = false;
}
void RoomMode::beginPreparation(uint64_t now) {
    std::lock_guard<std::mutex> l(mutex);
    if (s.active == "queue" && s.state == "prepare" && !prepareAt)
        prepareAt = now;
}
void RoomMode::stop() {
    std::lock_guard<std::mutex> l(mutex);
    stopped = true;
}
void RoomMode::evidence(ModeEvidence e, uint64_t now) {
    ModeStatus v;
    bool changed = false;
    {
        std::lock_guard<std::mutex> l(mutex);
        if (e.generation != s.generation || s.active != "queue" || s.state == "handoff")
            return;
        if (s.state == "drain" && s.item.occurrence && e.occurrence != s.item.occurrence && e.playing)
            e.ended = true;
        s.item = e;
        s.oldRate = s.newRate = e.rate;
        if (s.state == "prepare" && e.owned && e.playing) {
            s.state = "active";
            s.reason = "finite-ready";
            changed = true;
        }
        if (e.ended && !endAt)
            endAt = now;
        v = s;
    }
    if (changed)
        log(v);
    tick(now);
}
void RoomMode::request(const std::string &reason, uint64_t now) {
    ModeStatus v;
    {
        std::lock_guard<std::mutex> l(mutex);
        if (s.active != "queue" || s.latched)
            return;
        s.latched = true;
        s.lastFallback = reason;
        s.reason = reason;
        s.state = "drain";
        v = s;
    }
    log(v);
    tick(now);
}
void RoomMode::tick(uint64_t now) {
    ModeStatus v;
    bool timeout = false, cut = false;
    {
        std::lock_guard<std::mutex> l(mutex);
        if (s.active != "queue")
            return;
        if (s.state == "prepare" && prepareAt && now - prepareAt >= 15000)
            timeout = true;
        if (s.state != "drain") {
            if (!timeout)
                return;
        } else {
            cut = s.lastFallback == "foreign-control" || s.lastFallback == "coordinator-change" ||
                  !s.item.usable;
            if (s.item.owned && s.item.occurrence && !s.item.ended && (!cut || freezeFailed))
                return; // no timer-based early URI switch
            if (!endAt)
                endAt = now;
            s.state = "handoff";
            v = s;
        }
    }
    if (timeout) {
        request("preparation-timeout", now);
        return;
    }
    log(v);
    if (cut && v.item.owned && !v.item.ended && hooks.stopOwned) {
        try {
            hooks.stopOwned(v.item);
        } catch (const std::exception &e) {
            {
                std::lock_guard<std::mutex> l(mutex);
                s.state = "drain";
                freezeFailed = true;
                endAt = 0;
            }
            if (hooks.log)
                hooks.log("mode stop-unconfirmed=" + std::string(e.what()));
            return; // no URI switch and no repeated Stop; wait for confirmed end
        }
    }
    try {
        if (hooks.release)
            hooks.release(v.item);
    } catch (const std::exception &e) {
        if (hooks.log)
            hooks.log("mode cleanup-error=" + std::string(e.what()));
    }

    {
        std::lock_guard<std::mutex> l(mutex);
        s.item.owned = false;
        s.item.ahead = 0;
        s.active = "stream";
        s.oldRate = s.item.rate;
        s.newRate = 0;
        ++s.generation;
        v = s;
    }
    log(v);
    // Remains handoff until the first confirmed stream audio. Failure is
    // latched: no retry, reacquisition or implicit stream->queue switch.
    if (hooks.stream)
        hooks.stream(v.generation, v.item);
}
void RoomMode::streamAudible(uint64_t g, uint64_t now, unsigned rate) {
    ModeStatus v;
    {
        std::lock_guard<std::mutex> l(mutex);
        if (g != s.generation || s.active != "stream" || s.state != "handoff")
            return;
        s.gapMs = double(now >= endAt ? now - endAt : 0);
        s.state = "active";
        s.newRate = rate;
        v = s;
    }
    log(v);
}

} // namespace bridge
