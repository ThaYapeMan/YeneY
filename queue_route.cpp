// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "wav_writer.h"
#include <set>
#include <cstdio>
#include <utility>
namespace queue_audio {
namespace {
bool token(const std::string &s) {
    return !s.empty() &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
               std::string::npos;
}
} // namespace
struct Route::State {
    const std::string room, session;
    std::mutex mutex;
    struct Entry {
        uint64_t epoch, revision;
        std::shared_ptr<Item> item;
        Resolver resolve;
        std::function<std::shared_ptr<Item>()> ready;
    };
    std::map<std::string, Entry> entries;
    std::set<uint64_t> revoked;
    State(std::string room, std::string session) : room(std::move(room)), session(std::move(session)) {}
};
Route::Route(std::string r, std::string s) {
    if (r.empty() || !token(s))
        throw std::invalid_argument("invalid queue route owner");
    state = std::make_shared<State>(std::move(r), std::move(s));
}
void Route::registerWith(upnp::StreamServer &server) {
    // Each connection retains room state, even if the controller is released.
    // Revocation fences new readers while existing readers retain their asset.
    server.registerStream("queue", "/music/queue.wav", "YeneY queue", "audio/wav", "",
                          [held = state](upnp::StreamRequest &r) { return serve(held, r); });
}
std::string Route::publish(const std::string &owner, uint64_t epoch, uint64_t revision,
                           const std::shared_ptr<Item> &item, Resolver resolve) {
    if (owner != state->room || !item || item->room() != state->room || !item->contentLength() ||
        !token(item->descriptor().occurrence))
        throw std::invalid_argument("queue route ownership or unpublished item");
    const auto id = item->descriptor().occurrence;
    const auto key = std::to_string(epoch) + "/" + id + "/" + std::to_string(revision);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->revoked.count(epoch))
        throw std::logic_error("queue epoch revoked");
    const bool lazy = bool(resolve);
    auto inserted = state->entries.emplace(key, State::Entry{epoch, revision, item, std::move(resolve), {}});
    if (!inserted.second && (inserted.first->second.item != item || lazy || inserted.first->second.resolve))
        throw std::logic_error("immutable queue URL reused");
    return "/music/queue.wav?session=" + state->session + "&epoch=" + std::to_string(epoch) + "&item=" + id +
           "&revision=" + std::to_string(revision);
}
std::string Route::publishDeferred(uint64_t epoch, const std::string &identity, Resolver resolve, std::function<std::shared_ptr<Item>()> ready) {
    if (!token(identity) || !resolve) throw std::invalid_argument("invalid deferred route");
    const auto key = std::to_string(epoch) + "/" + identity + "/1";
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->revoked.count(epoch) || !state->entries.emplace(key, State::Entry{epoch, 1, {}, std::move(resolve), std::move(ready)}).second)
        throw std::logic_error("deferred route reused");
    return "/music/queue.wav?session=" + state->session + "&epoch=" + std::to_string(epoch) +
           "&item=" + identity + "&revision=1";
}
void Route::revoke(uint64_t epoch) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->revoked.insert(epoch);
    for (auto i = state->entries.begin(); i != state->entries.end();) {
        if (i->second.epoch == epoch)
            i = state->entries.erase(i);
        else
            ++i;
    }
}
bool Route::serve(upnp::StreamRequest &r) { return serve(state, r); }
bool Route::serve(const std::shared_ptr<State> &state, upnp::StreamRequest &r) {
    if (r.path() != "/music/queue.wav")
        return false;
    std::shared_ptr<Item> item;
    Resolver resolve;
    if (r.parameter("session") == state->session) {
        const auto key = r.parameter("epoch") + "/" + r.parameter("item") + "/" + r.parameter("revision");
        std::lock_guard<std::mutex> lock(state->mutex);
        auto i = state->entries.find(key);
        if (i != state->entries.end()) {
            item = i->second.item;
            resolve = i->second.resolve;
            if (!item && i->second.ready) {
                item = i->second.ready();
                if (item) resolve = {};
            }
        }
    }
    if (!item && resolve) {
        // Validate probes before enrolling a PCM reader. Bounds are checked
        // against the natural declaration once it is available.
        if (r.method() != upnp::StreamRequest::Method::Get) { r.reply(503); return true; }
        try {
            const auto range = wavRange(r.header("Range"), UINT64_MAX);
            if (range.last < 44 || range.first != 0) { r.reply(503); return true; }
        } catch (const std::exception &) { r.reply(416); return true; }
        try { item = resolve(r); } catch (const std::exception &) { }
        if (!item || r.aborted() || r.peerClosed()) { r.disconnect(); return true; }
        resolve = {};
    }
    if (!item) {
        auto id = r.parameter("item");
        if (id.empty() || id.size() > 32 || id.find_first_not_of("0123456789-_") != std::string::npos) id = "unknown";
        std::printf("yeney: queue HTTP method=%s item=%s range=unavailable status=404 content_length=0 produced_bytes=0 served_bytes=0 end=complete\n",
                    r.method() == upnp::StreamRequest::Method::Get ? "GET" :
                    r.method() == upnp::StreamRequest::Method::Head ? "HEAD" : "OTHER", id.c_str());
        r.reply(404);
        return true;
    }
    return serveWav(r, item, std::chrono::seconds(15), std::move(resolve));
}
} // namespace queue_audio
