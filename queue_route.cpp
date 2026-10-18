// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "wav_writer.h"
#include <set>
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
                           const std::shared_ptr<Item> &item) {
    if (owner != state->room || !item || item->room() != state->room || !item->contentLength() ||
        !token(item->descriptor().occurrence))
        throw std::invalid_argument("queue route ownership or unpublished item");
    const auto id = item->descriptor().occurrence;
    const auto key = std::to_string(epoch) + "/" + id + "/" + std::to_string(revision);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->revoked.count(epoch))
        throw std::logic_error("queue epoch revoked");
    auto inserted = state->entries.emplace(key, State::Entry{epoch, revision, item});
    if (!inserted.second && inserted.first->second.item != item)
        throw std::logic_error("immutable queue URL reused");
    return "/music/queue.wav?session=" + state->session + "&epoch=" + std::to_string(epoch) + "&item=" + id +
           "&revision=" + std::to_string(revision);
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
    if (r.parameter("session") == state->session) {
        const auto key = r.parameter("epoch") + "/" + r.parameter("item") + "/" + r.parameter("revision");
        std::lock_guard<std::mutex> lock(state->mutex);
        auto i = state->entries.find(key);
        if (i != state->entries.end())
            item = i->second.item;
    }
    if (!item) {
        r.reply(404);
        return true;
    }
    return serveWav(r, item);
}
} // namespace queue_audio
