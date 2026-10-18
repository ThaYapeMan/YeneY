// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "wav_writer.h"
#include <utility>
namespace queue_audio {
namespace {
bool token(const std::string &s) {
    return !s.empty() &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
               std::string::npos;
}
} // namespace
Route::Route(std::string r, std::string s) : room(std::move(r)), session(std::move(s)) {
    if (room.empty() || !token(session))
        throw std::invalid_argument("invalid queue route owner");
}
void Route::registerWith(upnp::StreamServer &server) {
    // Capture shared room state in the production owner; destroy the HTTP
    // listener before the route. Existing stream routes are unaffected.
    server.registerStream("queue", "/music/queue.wav", "YeneY queue", "audio/wav", "",
                          [this](upnp::StreamRequest &r) { return serve(r); });
}
std::string Route::publish(const std::string &owner, uint64_t epoch, uint64_t revision,
                           const std::shared_ptr<Item> &item) {
    if (owner != room || !item || !item->contentLength() || !token(item->descriptor().occurrence))
        throw std::invalid_argument("queue route ownership or unpublished item");
    const auto id = item->descriptor().occurrence;
    const auto key = std::to_string(epoch) + "/" + id + "/" + std::to_string(revision);
    std::lock_guard<std::mutex> lock(mutex);
    auto inserted = entries.emplace(key, Entry{epoch, revision, item});
    if (!inserted.second && inserted.first->second.item != item)
        throw std::logic_error("immutable queue URL reused");
    return "/music/queue.wav?session=" + session + "&epoch=" + std::to_string(epoch) + "&item=" + id +
           "&revision=" + std::to_string(revision);
}
void Route::revoke(uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto i = entries.begin(); i != entries.end();) {
        if (i->second.epoch == epoch)
            i = entries.erase(i);
        else
            ++i;
    }
}
bool Route::serve(upnp::StreamRequest &r) {
    if (r.path() != "/music/queue.wav")
        return false;
    std::shared_ptr<Item> item;
    if (r.parameter("session") == session) {
        const auto key = r.parameter("epoch") + "/" + r.parameter("item") + "/" + r.parameter("revision");
        std::lock_guard<std::mutex> lock(mutex);
        auto i = entries.find(key);
        if (i != entries.end())
            item = i->second.item;
    }
    if (!item) {
        r.reply(404);
        return true;
    }
    return serveWav(r, item);
}
} // namespace queue_audio
