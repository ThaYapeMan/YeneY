// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_controller.h"
#include "playback_mode.h"
#include "upnp/queue_coordinator.h"
#include <algorithm>
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
    uint64_t epoch = 0, lastEpoch = 0;
    std::vector<QueueAsset> plan;
    std::vector<std::string> urls;
    std::set<std::string> ownedUris;
    ControllerStatus report;
    std::thread worker;
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
          actions([this](const auto &name, const auto &args) { return coordinator.transport(name, args); }),
          ownership(ownershipIO(), options.session) {
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
        try {
            release("fault");
        } catch (const std::exception &e) {
            log("cleanup failed " + std::string(e.what()));
        }
        active = false;
        publishStatus("fault", message);
    }
    void release(const std::string &reason) {
        active = false;
        ownership.release();
        if (epoch)
            route.revoke(epoch);
        plan.clear();
        urls.clear();
        ownedUris.clear();
        started = false;
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
        assets.erase(assets.begin(), at);
        if (active && next == epoch) {
            setPlan(std::move(assets));
            return;
        }
        if (!next || next <= lastEpoch)
            throw StaleEpoch();
        release("new-play");
        epoch = lastEpoch = next;
        setPlan(std::move(assets));
        auto title = options.backupTitle ? options.backupTitle() : backupName();
        const auto saved = coordinator.playlists();
        auto base = title;
        unsigned suffix = 1;
        while (std::any_of(saved.begin(), saved.end(), [&](const auto &p) { return p.title == title; }))
            title = base + " (" + std::to_string(++suffix) + ")";
        ownership.acquire(title);
        active = true;
        publishStatus("owned");
        log("ownership acquired");
    }
    void pump() {
        if (!active)
            return;
        while (urls.size() < plan.size()) {
            const auto &asset = plan[urls.size()];
            if (!asset.item)
                break;
            auto length = asset.item->contentLength();
            if (!length)
                break;
            if (asset.item->descriptor().occurrence != std::to_string(asset.occurrence))
                throw std::invalid_argument("queue-asset-occurrence-mismatch");
            auto before = coordinator.browse();
            if (before.uris != urls)
                throw std::runtime_error("queue-changed-while-owned");
            auto metadata = asset.metadata;
            metadata.session = options.session;
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
            urls = std::move(expected);
            publishStatus("mirroring");
            log("mirrored occurrence=" + std::to_string(asset.occurrence) +
                " revision=" + std::to_string(asset.revision));
            if (!started) {
                actions.selectQueue(options.room.uuid);
                actions.play();
                started = true;
            }
        }
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
std::future<void> QueueController::release(std::string reason) {
    return impl->post([p = impl.get(), reason = std::move(reason)] { p->release(reason); });
}
ControllerStatus QueueController::status() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->report;
}
} // namespace queue_audio
