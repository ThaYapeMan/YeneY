// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "wav_writer.h"
#include "flac_writer.h"
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
    const Pacing pacing = Pacing::environment();
    const Format format;
    std::mutex mutex;
    struct Entry {
        uint64_t epoch, revision;
        Format format;
        std::shared_ptr<Item> item;
        Resolver resolve;
        Ready ready;
    };
    std::map<std::string, Entry> entries;
    std::set<uint64_t> revoked;
    std::string speakerAddress;
    std::function<void()> activity;
    State(std::string room, std::string session, Format format)
        : room(std::move(room)), session(std::move(session)), format(format) {}
};
Route::Route(std::string r, std::string s, Format format) {
    if (r.empty() || !token(s))
        throw std::invalid_argument("invalid queue route owner");
    state = std::make_shared<State>(std::move(r), std::move(s), format);
    state->pacing.log();
}
Format Route::formatFor(PromiseKind kind) const {
    return kind == PromiseKind::Exact ? state->format : Format::Wav;
}
void Route::registerWith(upnp::StreamServer &server, std::string address, std::function<void()> activity) {
    state->speakerAddress = std::move(address);
    state->activity = std::move(activity);
    // Each connection retains room state, even if the controller is released.
    // Revocation fences new readers while existing readers retain their asset.
    server.registerStream("queue", "/yeney/q/", "YeneY queue", "audio/wav", "",
                          [held = state](upnp::StreamRequest &r) { return serve(held, r); });
}
std::string Route::publish(const std::string &owner, uint64_t epoch, uint64_t revision,
                           const std::shared_ptr<Item> &item, Resolver resolve) {
    if (owner != state->room || !item || item->room() != state->room || !item->contentLength() ||
        !token(item->descriptor().occurrence))
        throw std::invalid_argument("queue route ownership or unpublished item");
    const auto id = item->descriptor().occurrence;
    const auto key = std::to_string(epoch) + "/" + std::to_string(revision) + "/" + id;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->revoked.count(epoch))
        throw std::logic_error("queue epoch revoked");
    const bool lazy = bool(resolve);
    const auto format = formatFor(item->descriptor().kind);
    auto inserted = state->entries.emplace(key, State::Entry{epoch, revision, format, item, std::move(resolve), {}});
    if (!inserted.second && (inserted.first->second.item != item || lazy || inserted.first->second.resolve))
        throw std::logic_error("immutable queue URL reused");
    return "/yeney/q/" + state->session + "/" + std::to_string(epoch) + "/" +
           std::to_string(revision) + "/" + id + formatExtension(format);
}
std::string Route::publishDeferred(uint64_t epoch, const std::string &identity, Resolver resolve, Ready ready, PromiseKind kind) {
    if (!token(identity) || !resolve) throw std::invalid_argument("invalid deferred route");
    const auto key = std::to_string(epoch) + "/1/" + identity;
    const auto format = formatFor(kind);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->revoked.count(epoch) || !state->entries.emplace(key, State::Entry{epoch, 1, format, {}, std::move(resolve), std::move(ready)}).second)
        throw std::logic_error("deferred route reused");
    return "/yeney/q/" + state->session + "/" + std::to_string(epoch) + "/1/" + identity + formatExtension(format);
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
    if (r.path().compare(0, 9, "/yeney/q/") != 0)
        return false;
    std::shared_ptr<Item> item;
    Resolver resolve;
    bool audioPinned = false;
    bool rowManaged = false;
    const auto path = r.path();
    const auto prefix = "/yeney/q/" + state->session + "/";
    std::string key;
    Format format = Format::Wav;
    const auto dot = path.rfind('.');
    if (path.compare(0, prefix.size(), prefix) == 0 && dot != std::string::npos && dot >= prefix.size())
        key = path.substr(prefix.size(), dot - prefix.size());
    if (!key.empty()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto i = state->entries.find(key);
        if (i != state->entries.end() && path.compare(dot, std::string::npos, formatExtension(i->second.format)) == 0) {
            format = i->second.format;
            if (state->activity && r.peerAddress() == state->speakerAddress &&
                (r.method() == upnp::StreamRequest::Method::Get || r.method() == upnp::StreamRequest::Method::Head))
                state->activity();
            item = i->second.item;
            resolve = i->second.resolve;
            rowManaged = bool(i->second.ready);
            if (!item && i->second.ready) {
                const bool audio = r.method() == upnp::StreamRequest::Method::Get;
                item = i->second.ready(audio);
                audioPinned = item && audio;
                if (audioPinned) resolve = {};
            }
        }
    }
    if (!item && resolve) {
        // Validate probes before enrolling a PCM reader. Bounds are checked
        // against the natural declaration once it is available.
        if (r.method() != upnp::StreamRequest::Method::Get) { r.reply(503); return true; }
        try {
            const auto range = wavRange(r.header("Range"), UINT64_MAX);
            if (range.last < (format == Format::Flac ? FlacLayout::headerSize : 44) || range.first != 0) { r.reply(503); return true; }
        } catch (const std::exception &) { r.reply(416); return true; }
        try { item = resolve(r); } catch (const std::exception &) { }
        if (!item || r.aborted() || r.peerClosed()) { r.disconnect(); return true; }
        resolve = {};
        audioPinned = rowManaged;
    }
    if (!item) {
        std::printf("yeney: queue HTTP method=%s item=unknown range=unavailable status=404 content_length=0 produced_bytes=0 served_bytes=0 end=complete\n",
                    r.method() == upnp::StreamRequest::Method::Get ? "GET" :
                    r.method() == upnp::StreamRequest::Method::Head ? "HEAD" : "OTHER");
        r.reply(404);
        return true;
    }
    if (format == Format::Flac && item->descriptor().kind != PromiseKind::Exact) {
        r.disconnect();
        return true;
    }
    return serveRow(r, item, format, std::chrono::seconds(15), std::move(resolve), audioPinned, state->pacing);
}
} // namespace queue_audio
