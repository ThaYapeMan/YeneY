// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_item_store.h"
#include "upnp/stream_server.h"
#include <condition_variable>
#include <algorithm>
#include <functional>
#include <mutex>

namespace queue_audio {
// A lazy route has a real immutable WAV declaration and a separately supplied
// producer. Selection is queued exactly once; neither HTTP nor HEAD executes
// an LMS command. The room callback must only enqueue work and return promptly.
class LazyItem {
    std::shared_ptr<Item> declaration, produced;
    std::function<void()> activate;
    std::mutex mutex;
    std::condition_variable changed;
    bool requested = false, cancelled = false;
public:
    LazyItem(std::shared_ptr<Item> declared, std::function<void()> enqueue)
        : declaration(std::move(declared)), activate(std::move(enqueue)) {
        if (!declaration || !declaration->contentLength() || !activate)
            throw std::invalid_argument("lazy item needs an immutable declaration and selector");
    }
    void publish(std::shared_ptr<Item> item) {
        if (!item || item->room() != declaration->room() ||
            item->descriptor().rate != declaration->descriptor().rate ||
            item->descriptor().kind != declaration->descriptor().kind ||
            item->contentLength() != declaration->contentLength())
            throw std::invalid_argument("lazy producer differs from declared WAV");
        std::lock_guard<std::mutex> lock(mutex);
        if (cancelled) return; // late producer cannot resurrect an abandoned window
        if (produced && produced != item) throw std::logic_error("lazy producer already bound");
        produced = std::move(item);
        changed.notify_all();
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex);
        cancelled = true;
        changed.notify_all();
    }
    std::shared_ptr<Item> resolve(upnp::StreamRequest &request,
                                 std::chrono::milliseconds budget = std::chrono::seconds(15)) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        std::unique_lock<std::mutex> lock(mutex);
        if (cancelled || request.aborted() || request.peerClosed()) return {};
        if (!produced && !requested) {
            requested = true;
            lock.unlock();
            try { activate(); }
            catch (...) { cancel(); throw; }
            lock.lock();
        }
        while (!produced && !cancelled && !request.aborted() && !request.peerClosed()) {
            if (std::chrono::steady_clock::now() >= deadline) return {};
            changed.wait_until(lock, std::min(deadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
        }
        return cancelled || request.aborted() || request.peerClosed() ? nullptr : produced;
    }
};
} // namespace queue_audio
