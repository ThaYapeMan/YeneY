// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_controller.h"
#include "playback_mode.h"
#include "position_state.h"
#include "upnp/queue_coordinator.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
namespace queue_audio {
namespace {
struct StaleEpoch : std::logic_error {
    StaleEpoch() : std::logic_error("queue-stale-epoch") {}
};
std::mutex roomsMutex;
std::set<std::string> rooms;
struct RoomLease {
    const std::string id;
    explicit RoomLease(std::string key) : id(std::move(key)) {
        std::lock_guard<std::mutex> lock(roomsMutex);
        if (!rooms.insert(id).second)
            throw std::runtime_error("queue-room-already-controlled");
    }
    ~RoomLease() {
        std::lock_guard<std::mutex> lock(roomsMutex);
        rooms.erase(id);
    }
};
std::string backupName() {
    const auto now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char text[64];
    std::strftime(text, sizeof(text), "Before YeneY %Y-%m-%d %H:%M", &tm);
    return text;
}
} // namespace
struct QueueController::Impl {
    ControllerOptions options;
    std::shared_ptr<upnp::StreamServer> server;
    RoomLease lease;
    Route route;
    upnp::QueueCoordinator coordinator;
    upnp::QueueActions actions;
    upnp::QueueOwnership ownership;
    struct Job {
        std::function<void()> run, cancel;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    bool closing = false, active = false, started = false;
    uint64_t epoch = 0, lastEpoch = 0, playlistRevision = 0;
    uint64_t target = 0, pendingSent = 0, intentDeadline = 0;
    bool rebuildAfterConfirm = false, awaitingFreshSeek = false;
    std::optional<uint32_t> seekAnchor;
    struct Expected {
        std::string value;
        uint64_t until = 0;
        bool take(const std::string &v) {
            if (until < nowMs() || value != v)
                return false;
            until = 0;
            return true;
        }
        void set(std::string v) {
            value = std::move(v);
            until = nowMs() + 5000;
        }
    };
    Expected ownState, echoState, echoToken, echoTime, echoRun;
    bool fallbackSent = false, cleaning = false, draining = false;
    uint32_t expectedUpdate = 0, observedMs = 0;
    uint64_t echoRunOccurrence = 0, ownStateOccurrence = 0;
    uint64_t observedAt = 0;
    bool appControl() const { return bool(options.fallbackRequest); }
    void foreign(const std::string &detail) {
        if (!active || fallbackSent)
            return;
        fallbackSent = true; // latch before cleanup or host callbacks can fail
        log("foreign-control detail=" + detail);
        if (options.managedFallback) {
            draining = true;
            options.fallbackRequest(detail == "group-rate-undecided"                  ? "coordinator-change"
                                    : detail.find("unconfirmed") != std::string::npos ? "preparation-timeout"
                                    : detail.find("soap") != std::string::npos        ? "soap-error"
                                                                                      : "foreign-control");
            return;
        }
        cleaning = true;
        try {
            release("foreign-control");
        } catch (const std::exception &e) {
            active = false;
            route.revoke(epoch);
            publishStatus("foreign-control", e.what());
            log("release failed " + std::string(e.what()));
        }
        cleaning = false;
        options.fallbackRequest("foreign-control");
    }
    bool forward(const std::string &command) {
        log("app LMS command=" + command);
        if (options.lmsControl && options.lmsControl(command))
            return true;
        foreign("lms-acknowledgement-failed");
        return false;
    }
    bool validateOwned() {
        if (!appControl() || draining)
            return true;
        try {
            coordinator.verifyLease();
            auto q = coordinator.browse();
            if (q.uris != urls || q.update != expectedUpdate) {
                foreign("queue-revision");
                return false;
            }
            if (actions.media().response.value("CurrentURI") !=
                "x-rincon-queue:" + options.room.uuid + "#0") {
                foreign("source");
                return false;
            }
            if (coordinator.transport("GetTransportSettings", {{"InstanceID", "0"}})
                        .response.value("PlayMode") != "NORMAL" ||
                actions.crossfade().response.value("CrossfadeMode") != "0") {
                foreign("play-mode");
                return false;
            }
        } catch (const std::exception &e) {
            foreign(e.what());
            return false;
        }
        return true;
    }
    std::vector<QueueAsset> playlist, pendingFuture;
    std::map<uint64_t, QueueAsset> history;
    static uint64_t token(const QueueAsset &a) { return a.token ? a.token : a.occurrence; }
    static bool available(const QueueAsset &a) {
        try {
            return a.item && a.item->contentLength().has_value();
        } catch (const std::exception &) {
            return false;
        }
    }
    std::vector<QueueAsset> plan;
    std::vector<std::string> urls;
    std::set<std::string> ownedUris;
    ControllerStatus report;
    std::thread worker;
    ConnectionPosition position;
    size_t audibleIndex = SIZE_MAX;
    unsigned clockEpoch = 0;
    yeney::PlaybackState speakerState = yeney::PlaybackState::Stopped;
    uint64_t lastFrames = 0;
    std::chrono::steady_clock::time_point pollAt{}, confirmedAt{};
    std::map<std::string, uint32_t> sequences;
    static uint64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    void ignored(const std::string &reason) { log("speaker ignored reason=" + reason); }
    size_t ownedIndex(const std::string &uri) const {
        auto it = std::find(urls.begin(), urls.end(), uri);
        return it == urls.end() ? SIZE_MAX : size_t(it - urls.begin());
    }
    static uint32_t decimal(const std::string &text) {
        uint64_t n = 0;
        if (text.empty())
            throw std::runtime_error("queue-position-invalid");
        for (char c : text) {
            if (c < '0' || c > '9' || n > (UINT32_MAX - unsigned(c - '0')) / 10)
                throw std::runtime_error("queue-position-invalid");
            n = n * 10 + unsigned(c - '0');
        }
        return uint32_t(n);
    }
    struct Reading {
        std::string uri, state;
        uint32_t track, ms;
    };
    Reading readPosition() {
        auto p = actions.position().response;
        auto time = p.value("RelTime");
        unsigned long long h, m, s;
        char tail;
        if (sscanf(time.c_str(), "%llu:%llu:%llu%c", &h, &m, &s, &tail) != 3 || h > UINT32_MAX / 3600000 ||
            m >= 60 || s >= 60 || (h * 3600 + m * 60 + s) * 1000 > UINT32_MAX)
            throw std::runtime_error("queue-position-invalid");
        return {p.value("TrackURI"), "", decimal(p.value("Track")), uint32_t((h * 3600 + m * 60 + s) * 1000)};
    }
    void confirm(const upnp::StateUpdate *event = nullptr) {
        if (!active || !started || !options.audiblePosition)
            return;
        if (!validateOwned())
            return;
        auto first = readPosition();
        auto state = coordinator.transport("GetTransportInfo", {{"InstanceID", "0"}})
                         .response.value("CurrentTransportState");
        auto p = readPosition(); // reject identity/state races across the SOAP pair
        if (first.uri != p.uri || first.track != p.track || first.ms != p.ms) {
            ignored("position-race");
            return;
        }
        if (appControl()) {
            const auto q = coordinator.browse();
            if (q.uris != urls || q.update != expectedUpdate) {
                foreign("queue-revision-during-evidence");
                return;
            }
        }
        auto index = ownedIndex(p.uri);
        if (index == SIZE_MAX || p.track != index + 1) {
            if (appControl())
                foreign("foreign-item");
            else
                ignored("foreign-or-revoked-uri");
            return;
        }
        if (event &&
            ((event->trackUri && *event->trackUri != p.uri) ||
             (event->trackNumber && *event->trackNumber != p.track) ||
             (event->positionMs && *event->positionMs != p.ms) || (event->state && *event->state != state))) {
            ignored("event-position-disagreement");
            return;
        }
        bool supersededTarget = false;
        if (target && plan[index].occurrence != target) {
            if (appControl() && event) {
                // A fresh matching event supersedes an older LMS intention.
                supersededTarget = true;
                target = pendingSent = 0;
                pendingFuture.clear();
                rebuildAfterConfirm = false;
            } else {
                if (appControl() && nowMs() > intentDeadline)
                    foreign("unconfirmed-target");
                else
                    ignored("pending-target");
                return;
            }
        }
        if (!appControl() && !target && audibleIndex != SIZE_MAX && index < audibleIndex) {
            ignored("older-item");
            return;
        }
        const bool commanded = target != 0;
        const bool newItem = index != audibleIndex;
        const bool progressingOwnSeek =
            appControl() && seekAnchor && p.ms >= *seekAnchor &&
            uint64_t(p.ms) <= uint64_t(*seekAnchor) + nowMs() + 6000 - intentDeadline;
        if (seekAnchor && (p.ms / 1000 != *seekAnchor / 1000) && !progressingOwnSeek) {
            if (appControl() && event)
                seekAnchor.reset();
            else {
                if (appControl() && nowMs() > intentDeadline)
                    foreign("unconfirmed-seek");
                else
                    ignored("pending-seek");
                return;
            }
        }
        if (newItem && state != "PLAYING") {
            if (appControl() && ownStateOccurrence == plan[index].occurrence && ownState.take(state)) {
                // Stop/pause can overtake confirmation of our own TRACK_NR.
                // Anchor the selected item without inventing an audible start;
                // CoreQueueSink waits for PLAYING before reporting its identity.
                audibleIndex = index;
                lastFrames = 0;
                if (options.reanchor)
                    options.reanchor(plan[index].occurrence, plan[index].sourceOriginMs);
                position.item(++clockEpoch, p.ms, plan[index].item->descriptor().rate, nowMs());
                speakerState =
                    state == "STOPPED" ? yeney::PlaybackState::Stopped : yeney::PlaybackState::Paused;
                confirmedAt = std::chrono::steady_clock::now();
                advance();
                return;
            }
            if (appControl() && ownState.until && nowMs() > ownState.until) {
                foreign("unconfirmed-state");
                return;
            }
            if (appControl() && state != "TRANSITIONING" && audibleIndex != SIZE_MAX) {
                foreign("ambiguous-paused-item");
                return;
            }
            ignored("not-playing");
            return;
        }
        yeney::PlaybackState next;
        if (state == "PLAYING")
            next = yeney::PlaybackState::Playing;
        else if (state == "PAUSED_PLAYBACK")
            next = yeney::PlaybackState::Paused;
        else if (state == "STOPPED")
            next = yeney::PlaybackState::Stopped;
        else
            return; // TRANSITIONING is not proof of an audible start
        const auto now = nowMs();
        bool appSeek = false;
        if (appControl() && !draining && audibleIndex != SIZE_MAX) {
            if ((newItem || supersededTarget) && !commanded) {
                const auto &old = plan[audibleIndex];
                const auto duration =
                    (*old.item->contentLength() - 44) / 6 * 1000 / old.item->descriptor().rate;
                const auto projected =
                    observedMs + (speakerState == yeney::PlaybackState::Playing ? now - observedAt : 0);
                const bool natural = !supersededTarget && index == audibleIndex + 1 &&
                                     projected + 1500 >= duration && p.ms <= 1000;
                if (!natural) {
                    if ((!supersededTarget && index + 1 != audibleIndex && index != audibleIndex + 1) ||
                        !available(plan[index])) {
                        foreign("ambiguous-item-jump");
                        return;
                    }
                    auto at = std::find_if(playlist.begin(), playlist.end(),
                                           [&](const auto &a) { return token(a) == token(plan[index]); });
                    if (at == playlist.end() ||
                        std::count_if(playlist.begin(), playlist.end(),
                                      [&](const auto &a) { return token(a) == token(plan[index]); }) != 1) {
                        foreign("unmapped-token");
                        return;
                    }
                    echoToken.set(std::to_string(token(*at)));
                    echoRun.set("f");
                    echoRunOccurrence = plan[index].occurrence;
                    if (!forward("playlist index " + std::to_string(at - playlist.begin())))
                        return;
                    log("app skip token=" + std::to_string(token(*at)) +
                        " mixed-intro=" + std::to_string(plan[index].mixedIntro));
                }
            }
            const bool contrary = event && ownState.until >= now && ownState.value != state;
            const bool own = ownState.take(state);
            if (contrary)
                ownState.until = 0;
            if (!event && ownState.until && !own && ownState.value != state) {
                if (now > ownState.until)
                    foreign("unconfirmed-state");
                else
                    ignored("pending-state");
                return;
            }
            if ((next != speakerState || contrary) && !own && !commanded) {
                if (next == yeney::PlaybackState::Stopped) {
                    foreign("unmapped-stop");
                    return;
                }
                echoState.set(next == yeney::PlaybackState::Paused ? "p" : "u");
                if (!forward(next == yeney::PlaybackState::Paused ? "pause 1" : "play"))
                    return;
            }
            if (!newItem && !commanded && !seekAnchor && observedAt) {
                const int64_t projected =
                    observedMs + (speakerState == yeney::PlaybackState::Playing ? now - observedAt : 0);
                const auto difference = int64_t(p.ms) - projected;
                appSeek = std::abs(difference) > 1500 ||
                          (speakerState != yeney::PlaybackState::Playing && p.ms != observedMs);
                if (appSeek) {
                    const auto &a = plan[index];
                    const auto frame = uint64_t(p.ms) * a.item->descriptor().rate / 1000;
                    if (frame >= (*a.item->contentLength() - 44) / 6) {
                        foreign("seek-outside-item");
                        return;
                    }
                    const bool retained = frame < a.item->producedFrames();
                    const auto original = a.occurrence;
                    if (uint64_t(p.ms) + a.sourceOriginMs > UINT32_MAX) {
                        foreign("unmapped-source-time");
                        return;
                    }
                    const auto sourceMs = p.ms + a.sourceOriginMs;
                    if (!forward("time " + std::to_string(sourceMs / 1000)))
                        return;
                    if (!retained) {
                        // The CLI refetch supplies a new occurrence through the
                        // existing nonblocking preparation/snapshot boundary.
                        seek(sourceMs);
                        awaitingFreshSeek = true;
                        echoTime.set(std::to_string(sourceMs));
                        echoRun.set("f");
                        echoRunOccurrence = original;
                        log("app seek history=rerender source-ms=" + std::to_string(sourceMs));
                        return; // old item/time is not evidence for the new asset
                    }
                    echoTime.set(std::to_string(sourceMs));
                    echoRun.set("f");
                    echoRunOccurrence = original;
                }
            }
        } else if (appControl())
            ownState.take(state);
        observedMs = p.ms;
        observedAt = now;
        if (newItem || commanded || seekAnchor || appSeek) {
            if (rebuildAfterConfirm) {
                removeFuture(index + 1);
                plan.resize(index + 1);
                plan.insert(plan.end(), pendingFuture.begin(), pendingFuture.end());
                pendingFuture.clear();
                rebuildAfterConfirm = false;
                log("rebuild confirmed-target future=" + std::to_string(plan.size() - index - 1));
            }
            target = 0;
            awaitingFreshSeek = false;
            seekAnchor.reset();
            if (options.reanchor)
                options.reanchor(plan[index].occurrence, plan[index].sourceOriginMs);
            audibleIndex = index;
            lastFrames = 0;
            position.item(++clockEpoch, p.ms, plan[index].item->descriptor().rate, now);
            const auto at = plan[index].strmAt;
            log("audible occurrence=" + std::to_string(plan[index].occurrence) + " track=" +
                std::to_string(p.track) + " RelTime=" + std::to_string(p.ms) + " strm_to_audible_s=" +
                (at.time_since_epoch().count()
                     ? std::to_string(
                           std::chrono::duration<double>(std::chrono::steady_clock::now() - at).count())
                     : "unknown"));
        } else {
            // Resume reanchors the frozen position; paused wall time is excluded.
            if (speakerState != yeney::PlaybackState::Playing && next == yeney::PlaybackState::Playing)
                position.item(++clockEpoch, p.ms, plan[index].item->descriptor().rate, now, lastFrames);
            else
                position.itemPoll(p.ms, now);
        }
        if (next != yeney::PlaybackState::Playing)
            lastFrames = std::max(lastFrames, uint64_t(p.ms) * plan[index].item->descriptor().rate / 1000);
        speakerState = next;
        confirmedAt = std::chrono::steady_clock::now();
        if (options.confirmed) {
            const auto &a = plan[index];
            options.confirmed({epoch, a.occurrence, uint64_t(p.ms) * a.item->descriptor().rate / 1000,
                               (*a.item->contentLength() - 44) / 6, a.item->producedFrames(),
                               a.item->descriptor().rate, p.ms + a.sourceOriginMs,
                               unsigned(urls.size() - index - 1), next});
        }
        advance();
    }
    void advance() {
        if (!active || awaitingFreshSeek || audibleIndex == SIZE_MAX || !options.audiblePosition)
            return;
        // Stop projecting if position evidence is lost; never invent ongoing
        // playback after a disconnected speaker. Ordinary polls refresh at 1 Hz.
        if (std::chrono::steady_clock::now() - confirmedAt > std::chrono::seconds(2))
            return;
        const auto &asset = plan[audibleIndex];
        auto frames = lastFrames;
        if (speakerState == yeney::PlaybackState::Playing)
            frames = position.smoothFrames(asset.item->descriptor().rate, double(nowMs()), false, 0,
                                           options.deviceRatePrior);
        frames = std::min((*asset.item->contentLength() - 44) / 6, std::max(lastFrames, frames));
        lastFrames = frames;
        options.audiblePosition(asset.occurrence, frames, speakerState);
    }
    void notify(uint64_t receivedEpoch, const upnp::GenaEvent &event) {
        if (!active || receivedEpoch != epoch) {
            ignored("stale-epoch");
            return;
        }
        if (event.service != upnp::Service::AVTransport)
            return;
        upnp::StateUpdate update;
        if (!upnp::parseAvTransport(event.body, update, true)) {
            ignored("invalid-event");
            return;
        }
        if (!appControl() && update.trackUri && ownedIndex(*update.trackUri) == SIZE_MAX) {
            ignored("foreign-or-revoked-uri");
            return;
        }
        auto prior = sequences.find(event.sid);
        if (prior != sequences.end() &&
            (event.sequence == prior->second || uint32_t(event.sequence - prior->second) > INT32_MAX)) {
            ignored("stale-sequence");
            return;
        }
        sequences[event.sid] = event.sequence;
        confirm(&update);
    }
    upnp::QueueOwnershipIO ownershipIO() {
        upnp::QueueOwnershipIO io;
        io.browse = [this] { return coordinator.browse(); };
        io.owned = [this](const std::string &uri) { return ownedUris.count(uri) != 0; };
        io.playlists = [this] { return coordinator.playlists(); };
        io.save = [this](const std::string &title) {
            return actions.save(title).response.value("AssignedObjectID");
        };
        io.deletePlaylist = [this](const std::string &id) { coordinator.destroyPlaylist(id); };
        io.crossfade = [this] {
            auto value = actions.crossfade().response.value("CrossfadeMode");
            if (value != "0" && value != "1")
                throw std::runtime_error("queue-invalid-crossfade-mode");
            return value == "1";
        };
        io.setCrossfade = [this](bool value) {
            actions.crossfade(value);
            const auto confirmed = actions.crossfade().response.value("CrossfadeMode");
            if (confirmed != (value ? "1" : "0"))
                throw std::runtime_error("queue-crossfade-unconfirmed");
        };
        io.remove = [this](uint32_t update, unsigned first, unsigned count) {
            actions.remove(update, first, count);
        };
        io.log = [this](const std::string &text) { log(text); };
        return io;
    }
    Impl(ControllerOptions opts, std::shared_ptr<upnp::StreamServer> http)
        : options(std::move(opts)), server(std::move(http)), lease(options.room.uuid),
          route(options.room.name, options.session), coordinator(options.room, options.log),
          actions([this](const auto &name, const auto &args) {
              return cleaning ? coordinator.releaseTransport(name, args) : coordinator.transport(name, args);
          }),
          ownership(ownershipIO(), options.session) {
        if (!std::isfinite(options.deviceRatePrior) || options.deviceRatePrior <= 0)
            throw std::invalid_argument("queue-invalid-device-rate");
        if (!server || options.httpBase.compare(0, 7, "http://") || options.httpBase.back() == '/')
            throw std::invalid_argument("queue-invalid-http-base");
        upnp::HttpUrl base;
        if (!upnp::parseHttpUrl(options.httpBase, base) || base.path != "/")
            throw std::invalid_argument("queue-invalid-http-base");
        route.registerWith(*server);
        worker = std::thread([this] { work(); });
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
        }
        changed.notify_all();
        worker.join();
    }
    void log(const std::string &text) const {
        if (options.log)
            options.log("queue room=" + options.room.name + " epoch=" + std::to_string(epoch) + " " + text);
    }
    void publishStatus(const std::string &reason, const std::string &fault = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        report = {active, epoch, urls.size(), reason, fault};
    }
    std::future<void> post(std::function<void()> run) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        std::lock_guard<std::mutex> lock(mutex);
        if (closing || jobs.size() >= 128)
            throw std::runtime_error("queue-controller-unavailable");
        jobs.push_back({[this, run = std::move(run), promise] {
                            try {
                                run();
                                pump();
                                promise->set_value();
                            } catch (const StaleEpoch &) {
                                promise->set_exception(std::current_exception());
                            } catch (...) {
                                auto error = std::current_exception();
                                fault(error);
                                promise->set_exception(error);
                            }
                        },
                        [promise] {
                            promise->set_exception(
                                std::make_exception_ptr(std::runtime_error("queue-controller-shutdown")));
                        }});
        changed.notify_all();
        return future;
    }
    void work() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(50),
                                 [&] { return closing || !jobs.empty(); });
                if (closing) {
                    for (auto &pending : jobs)
                        pending.cancel();
                    jobs.clear();
                    break;
                }
                if (!jobs.empty()) {
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
            }
            if (job.run)
                job.run();
            else
                try {
                    pump();
                } catch (...) {
                    fault(std::current_exception());
                }
        }
        try {
            release("service-shutdown");
        } catch (const std::exception &e) {
            log("release failed " + std::string(e.what()));
        }
    }
    void fault(std::exception_ptr error) {
        std::string message = "queue-controller-error";
        try {
            std::rethrow_exception(error);
        } catch (const std::exception &e) {
            message = e.what();
        } catch (...) {
        }
        log("fault=" + message);
        if (options.managedFallback && active) {
            if (!fallbackSent) {
                fallbackSent = true;
                draining = true;
                const auto reason = message.find("length") != std::string::npos ||
                                            message.find("frame count") != std::string::npos
                                        ? "length-fault"
                                    : message.find("quota") != std::string::npos ||
                                            message.find("history") != std::string::npos ||
                                            message.find("store") != std::string::npos
                                        ? "storage-fault"
                                    : message.find("unconfirmed") != std::string::npos ? "preparation-timeout"
                                    : message.find("group") != std::string::npos       ? "coordinator-change"
                                                                                       : "soap-error";
                options.fallbackRequest(reason);
            }
            return;
        }
        if (appControl() && active) {
            foreign(message);
            return;
        }
        try {
            release("fault");
        } catch (const std::exception &e) {
            log("cleanup failed " + std::string(e.what()));
        }
        active = false;
        publishStatus("fault", message);
    }
    void release(const std::string &reason) {
        if (fallbackSent && !active)
            return;
        active = false;
        if (options.managedFallback)
            cleaning = true;
        try {
            ownership.release();
        } catch (...) {
            cleaning = false;
            draining = false;
            if (epoch)
                route.revoke(epoch);
            throw;
        }
        cleaning = false;
        draining = false;
        if (epoch)
            route.revoke(epoch);
        plan.clear();
        playlist.clear();
        history.clear();
        playlistRevision = target = pendingSent = 0;
        seekAnchor.reset();
        pendingFuture.clear();
        rebuildAfterConfirm = false;
        awaitingFreshSeek = false;
        urls.clear();
        ownedUris.clear();
        started = false;
        audibleIndex = SIZE_MAX;
        lastFrames = 0;
        sequences.clear();
        speakerState = yeney::PlaybackState::Stopped;
        publishStatus(reason);
        log("release reason=" + reason);
    }
    void setPlan(std::vector<QueueAsset> assets) {
        if (assets.empty() || assets.size() > 60000)
            throw std::invalid_argument("queue-invalid-window");
        std::set<uint64_t> seen;
        for (const auto &a : assets)
            if (!a.occurrence || !seen.insert(a.occurrence).second)
                throw std::invalid_argument("queue-invalid-occurrence");
        if (assets.size() < urls.size())
            throw std::invalid_argument("queue-published-plan-changed");
        for (size_t i = 0; i < urls.size(); ++i)
            if (assets[i].occurrence != plan[i].occurrence || assets[i].revision != plan[i].revision ||
                assets[i].item != plan[i].item)
                throw std::invalid_argument("queue-published-plan-changed");
        plan = std::move(assets);
    }
    void play(uint64_t next, uint64_t current, std::vector<QueueAsset> assets) {
        if (!next || next < lastEpoch || (next == lastEpoch && !active))
            throw StaleEpoch();
        auto at = std::find_if(assets.begin(), assets.end(),
                               [&](const auto &a) { return a.occurrence == current; });
        if (at == assets.end())
            throw std::invalid_argument("queue-current-occurrence-missing");
        auto complete = assets;
        assets.erase(assets.begin(), at);
        if (active && next == epoch) {
            setPlan(std::move(assets));
            return;
        }
        if (!next || next <= lastEpoch)
            throw StaleEpoch();
        release("new-play");
        fallbackSent = false;
        observedAt = 0;
        ownState = echoState = echoToken = echoTime = echoRun = {};
        epoch = lastEpoch = next;
        playlist = complete;
        playlistRevision = 1;
        for (const auto &a : complete)
            history[token(a)] = a;
        setPlan(std::move(assets));
        auto title = options.backupTitle ? options.backupTitle() : backupName();
        const auto saved = coordinator.playlists();
        auto base = title;
        unsigned suffix = 1;
        while (std::any_of(saved.begin(), saved.end(), [&](const auto &p) { return p.title == title; }))
            title = base + " (" + std::to_string(++suffix) + ")";
        ownership.acquire(title);
        expectedUpdate = coordinator.browse().update;
        active = true;
        publishStatus("owned");
        log("ownership acquired");
    }
    void checkEpoch(uint64_t received) {
        if (!active || received != epoch)
            throw StaleEpoch();
    }
    void removeFuture(size_t keep) {
        auto before = coordinator.browse();
        if (before.uris != urls || (appControl() && before.update != expectedUpdate))
            throw std::runtime_error("queue-changed-while-owned");
        if (urls.size() > keep) {
            actions.remove(before.update, unsigned(keep + 1), unsigned(urls.size() - keep));
            auto expected = std::vector<std::string>(urls.begin(), urls.begin() + keep);
            auto after = coordinator.browse();
            if (after.uris != expected || after.update == before.update)
                throw std::runtime_error("queue-rebuild-unconfirmed");
            expectedUpdate = after.update;
            urls = std::move(expected);
        }
    }
    void reconcile(uint64_t revision, uint64_t current, std::vector<QueueAsset> assets, bool select,
                   bool alreadyPrepared = false) {
        if (!revision || revision < playlistRevision)
            throw StaleEpoch();
        std::set<uint64_t> seen;
        if (assets.size() > 60000)
            throw std::invalid_argument("queue-invalid-window");
        for (const auto &a : assets) {
            if (!token(a) || !a.occurrence || !seen.insert(token(a)).second)
                throw std::invalid_argument("queue-invalid-occurrence");
        }
        if (select && echoToken.take(std::to_string(current))) {
            select = false; // retain any simultaneous authoritative playlist edit
            log("echo suppressed LMS playlist token=" + std::to_string(current));
        }
        if (!select && revision == playlistRevision && assets.size() == playlist.size() &&
            std::equal(assets.begin(), assets.end(), playlist.begin(), [&](const auto &a, const auto &b) {
                return token(a) == token(b) && a.occurrence == b.occurrence && a.revision == b.revision &&
                       a.item == b.item;
            }))
            return;
        for (const auto &a : plan)
            history[token(a)] = a;
        for (auto &a : assets) {
            auto old = history.find(token(a));
            if ((!alreadyPrepared || token(a) != current) && !a.item && old != history.end() &&
                available(old->second))
                a = old->second;
        }
        auto chosen =
            std::find_if(assets.begin(), assets.end(), [&](const auto &a) { return token(a) == current; });
        if (select && chosen == assets.end())
            throw std::invalid_argument("queue-current-occurrence-missing");
        if (select && !alreadyPrepared && !available(*chosen)) {
            if (!options.prepare)
                throw std::runtime_error("queue-preparation-unavailable");
            auto prepared = options.prepare(current, 0);
            prepared.token = current;
            *chosen = std::move(prepared);
            log("prepare token=" + std::to_string(current) + " reason=history-missing");
        }
        if (select)
            echoRun.until = 0;
        playlist = assets;
        playlistRevision = revision;
        if (!select) {
            // Confirm the audible identity immediately before future-only mutation.
            auto p = readPosition();
            auto index = ownedIndex(p.uri);
            if (index == SIZE_MAX || p.track != index + 1 ||
                (audibleIndex != SIZE_MAX && index != audibleIndex))
                throw std::runtime_error("queue-rebuild-unsafe-position");
            const auto audibleToken = token(plan[index]);
            auto next = std::find_if(assets.begin(), assets.end(),
                                     [&](const auto &a) { return token(a) == audibleToken; });
            std::vector<QueueAsset> future;
            if (next != assets.end())
                future.assign(std::next(next), assets.end());
            else if (chosen != assets.end())
                future.assign(chosen,
                              assets.end()); // LMS supplies the successor after deleting current
            else
                future = assets; // clear never removes the audible asset
            removeFuture(index + 1);
            plan.resize(index + 1);
            plan.insert(plan.end(), future.begin(), future.end());
            log("rebuild revision=" + std::to_string(revision) + " audible-token=" +
                std::to_string(audibleToken) + " future=" + std::to_string(future.size()));
            return;
        }
        // Keep the audible item and all history before it. The prepared target
        // is appended if it was removed, retaining the immutable cached intro.
        auto old = std::find_if(plan.begin(), plan.begin() + urls.size(), [&](const auto &a) {
            return a.occurrence == chosen->occurrence && a.item == chosen->item;
        });
        const bool cached = old != plan.begin() + urls.size();
        pendingFuture.clear();
        rebuildAfterConfirm = false;
        seekAnchor.reset();
        if (cached) {
            pendingFuture.assign(std::next(chosen), assets.end());
            rebuildAfterConfirm = true;
        } else {
            size_t keep = audibleIndex == SIZE_MAX ? urls.size() : audibleIndex + 1;
            removeFuture(keep);
            plan.resize(keep);
            plan.insert(plan.end(), chosen, assets.end());
        }
        target = chosen->occurrence;
        pendingSent = 0;
        intentDeadline = nowMs() + 15000; // publication/refetch precedes the
                                          // five-second SOAP acknowledgement
        log("rebuild revision=" + std::to_string(revision) +
            " select-token=" + std::to_string(current) + " cached=" + std::to_string(cached) +
            " mixed-intro=" + std::to_string(chosen->mixedIntro));
    }
    void command(char command) {
        if ((command == 'f' || command == 's') && audibleIndex != SIZE_MAX &&
            plan[audibleIndex].occurrence == echoRunOccurrence && echoRun.take(std::string(1, command))) {
            if (command == 'f')
                echoRun.set("s");
            log("echo suppressed LMS retained-run=" + std::string(1, command));
            return;
        }
        if (echoState.take(std::string(1, command))) {
            log("echo suppressed LMS strm=" + std::string(1, command));
            return;
        }
        if (command != 's')
            echoRun.until = 0;
        ownStateOccurrence =
            target ? target
                   : (audibleIndex != SIZE_MAX ? plan[audibleIndex].occurrence : plan.front().occurrence);
        if (command == 'p')
            ownState.set("PAUSED_PLAYBACK");
        else if (command == 'u')
            ownState.set("PLAYING");
        else if (command == 'q' || command == 'f')
            ownState.set("STOPPED");
        if (command == 'p')
            actions.pause();
        else if (command == 'u')
            actions.play();
        else if (command == 'q' || command == 'f') {
            target = pendingSent = 0;
            pendingFuture.clear();
            rebuildAfterConfirm = false;
            seekAnchor.reset();
            actions.stop();
        } else if (command != 's')
            throw std::invalid_argument("queue-invalid-command");
        // s identity is supplied by lmsPlaylist, never inferred from read-ahead.
        log("LMS strm=" + std::string(1, command) + " ownership=retained");
        pollAt = {};
    }
    void seek(uint32_t ms) {
        if (echoTime.take(std::to_string(ms))) {
            log("echo suppressed LMS time=" + std::to_string(ms));
            return;
        }
        echoRun.until = 0;
        if (audibleIndex == SIZE_MAX)
            throw std::runtime_error("queue-no-audible-item");
        auto &a = plan[audibleIndex];
        if (ms >= a.sourceOriginMs && available(a) &&
            uint64_t(ms - a.sourceOriginMs) * a.item->descriptor().rate / 1000 < a.item->producedFrames()) {
            const auto relative = (ms - a.sourceOriginMs) / 1000;
            actions.seekTime(std::to_string(relative / 3600) + ":" + std::to_string(relative / 60 % 60) +
                             ":" + std::to_string(relative % 60));
            seekAnchor = relative * 1000;
            intentDeadline = nowMs() + 5000;
            log("seek token=" + std::to_string(token(a)) + " source-ms=" + std::to_string(ms) +
                " history=retained");
        } else {
            if (!options.prepare)
                throw std::runtime_error("queue-preparation-unavailable");
            auto replacement = options.prepare(token(a), ms);
            if (!replacement.occurrence || replacement.occurrence == a.occurrence)
                throw std::runtime_error("queue-seek-requires-new-occurrence");
            replacement.token = token(a);
            replacement.sourceOriginMs = ms;
            auto assets = playlist;
            auto it = std::find_if(assets.begin(), assets.end(),
                                   [&](const auto &x) { return token(x) == token(a); });
            if (it == assets.end())
                throw std::runtime_error("queue-seek-item-removed");
            *it = replacement;
            reconcile(playlistRevision, token(a), std::move(assets), true, true);
            awaitingFreshSeek = true; // the superseded producer may now cancel its old asset
            log("seek source-ms=" + std::to_string(ms) +
                " history=rerender new-occurrence=" + std::to_string(replacement.occurrence));
        }
        pollAt = {};
    }
    void pump() {
        if (!active)
            return;
        if (draining) {
            if (options.audiblePosition && started && std::chrono::steady_clock::now() >= pollAt) {
                pollAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                confirm();
            }
            advance();
            return;
        }
        while (urls.size() < plan.size()) {
            auto &asset = plan[urls.size()];
            if (!asset.item)
                break;
            std::optional<uint64_t> length;
            try {
                length = asset.item->contentLength();
            } catch (const PublicationSuperseded &) {
                break; // host snapshot will supply the immutable published handle
            }
            if (!length)
                break;
            if (asset.item->descriptor().occurrence != std::to_string(asset.occurrence))
                throw std::invalid_argument("queue-asset-occurrence-mismatch");
            auto before = coordinator.browse();
            if (before.uris != urls || (appControl() && before.update != expectedUpdate))
                throw std::runtime_error("queue-changed-while-owned");
            auto metadata = asset.metadata;
            metadata.session = options.session;
            for (size_t i = 0; i < urls.size(); ++i)
                if (plan[i].occurrence == asset.occurrence && plan[i].revision >= asset.revision)
                    asset.revision = plan[i].revision + 1;
            metadata.url =
                options.httpBase + route.publish(options.room.name, epoch, asset.revision, asset.item);
            metadata.size = *length;
            metadata.frames = (*length - 44) / 6;
            metadata.rate = asset.item->descriptor().rate;
            ownedUris.insert(metadata.url); // also clean up an enqueue whose reply is lost
            auto result = actions.addMultiple({{metadata.url, upnp::queueDidl(metadata)}}, before.update);
            auto expected = urls;
            expected.push_back(metadata.url);
            auto after = coordinator.browse();
            if (result.response.value("NumTracksAdded") != "1" || after.uris != expected ||
                after.update == before.update)
                throw std::runtime_error("queue-enqueue-unconfirmed");
            expectedUpdate = after.update;
            urls = std::move(expected);
            publishStatus("mirroring");
            log("mirrored occurrence=" + std::to_string(asset.occurrence) +
                " revision=" + std::to_string(asset.revision));
            if (!started) {
                actions.selectQueue(options.room.uuid);
                actions.seekTrack(1);
                ownState.set("PLAYING");
                actions.play();
                started = true;
            }
        }
        if (target && pendingSent != target) {
            auto it = std::find_if(plan.begin(), plan.begin() + urls.size(),
                                   [&](const auto &a) { return a.occurrence == target; });
            if (it != plan.begin() + urls.size()) {
                actions.seekTrack(unsigned(it - plan.begin() + 1));
                ownState.set("PLAYING");
                actions.play();
                // Keep target as an evidence fence, but issue the action once.
                pendingSent = target;
                intentDeadline = nowMs() + 5000;
            }
        }
        if (options.audiblePosition && started && std::chrono::steady_clock::now() >= pollAt) {
            pollAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            confirm();
        }
        advance();
    }
};
QueueController::QueueController(ControllerOptions options, std::shared_ptr<upnp::StreamServer> server) {
#ifndef YENEY_QUEUE_CONTROLLER_TEST
    if (!bridge::queuePlaybackReady)
        throw std::runtime_error("queue-unavailable");
#endif
    if (bridge::selectPlaybackMode("queue", options.room, true).selected != bridge::PlaybackMode::Queue)
        throw std::invalid_argument("group-rate-undecided");
    impl = std::make_unique<Impl>(std::move(options), std::move(server));
}
QueueController::~QueueController() = default;
std::future<void> QueueController::play(uint64_t epoch, uint64_t current, std::vector<QueueAsset> assets) {
    return impl->post([p = impl.get(), epoch, current, assets = std::move(assets)]() mutable {
        p->play(epoch, current, std::move(assets));
    });
}
std::future<void> QueueController::update(uint64_t epoch, std::vector<QueueAsset> assets) {
    return impl->post([p = impl.get(), epoch, assets = std::move(assets)]() mutable {
        if (!p->active || epoch != p->epoch)
            throw StaleEpoch();
        p->setPlan(std::move(assets));
    });
}
std::future<void> QueueController::prepared(uint64_t epoch, std::vector<QueueAsset> assets) {
    return impl->post([p = impl.get(), epoch, assets = std::move(assets)] {
        p->checkEpoch(epoch);
        for (const auto &a : assets) {
            for (size_t i = p->urls.size(); i < p->plan.size(); ++i)
                if (p->plan[i].occurrence == a.occurrence)
                    p->plan[i] = a;
            for (auto &entry : p->playlist)
                if (entry.occurrence == a.occurrence)
                    entry = a;
        }
    });
}
std::future<void> QueueController::lmsPlaylist(uint64_t epoch, uint64_t revision, uint64_t current,
                                               std::vector<QueueAsset> assets, bool select) {
    return impl->post(
        [p = impl.get(), epoch, revision, current, assets = std::move(assets), select]() mutable {
            p->checkEpoch(epoch);
            p->reconcile(revision, current, std::move(assets), select);
        });
}
std::future<void> QueueController::lmsCommand(uint64_t epoch, char command) {
    return impl->post([p = impl.get(), epoch, command] {
        p->checkEpoch(epoch);
        p->command(command);
    });
}
std::future<void> QueueController::lmsSeek(uint64_t epoch, uint32_t ms) {
    return impl->post([p = impl.get(), epoch, ms] {
        p->checkEpoch(epoch);
        p->seek(ms);
    });
}
std::future<void> QueueController::freeze(uint64_t epoch) {
    return impl->post([p = impl.get(), epoch] {
        p->checkEpoch(epoch);
        auto position = p->readPosition();
        if (!p->ownedUris.count(position.uri))
            return;
        p->actions.stop();
        if (p->coordinator.transport("GetTransportInfo", {{"InstanceID", "0"}})
                .response.value("CurrentTransportState") != "STOPPED")
            throw std::runtime_error("queue-stop-unconfirmed");
    });
}
std::future<void> QueueController::drain(uint64_t epoch) {
    return impl->post([p = impl.get(), epoch] {
        p->checkEpoch(epoch);
        p->draining = true;
        if (p->audibleIndex != SIZE_MAX) {
            p->removeFuture(p->audibleIndex + 1);
            p->plan.resize(p->audibleIndex + 1);
        }
    });
}
std::future<void> QueueController::release(std::string reason) {
    return impl->post([p = impl.get(), reason = std::move(reason)] { p->release(reason); });
}
std::future<void> QueueController::notify(uint64_t epoch, upnp::GenaEvent event) {
    return impl->post([p = impl.get(), epoch, event = std::move(event)] { p->notify(epoch, event); });
}
ControllerStatus QueueController::status() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->report;
}
} // namespace queue_audio
