// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include <algorithm>
#include <cmath>
namespace queue_audio {
double CoreQueueSink::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count() * clockSpeed;
}
CoreQueueSink::CoreQueueSink(const std::string &room, Config c, double seconds, double speed)
    : store(room, c), budget(seconds), clockSpeed(speed) {
    if (!std::isfinite(speed) || speed <= 0)
        throw std::invalid_argument("invalid queue clock speed");
    if (!std::isfinite(seconds) || seconds < 1 || seconds > 600)
        throw std::invalid_argument("invalid queue job seconds");
    worker = std::thread([this] { work(); });
}
CoreQueueSink::~CoreQueueSink() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        closing = true;
    }
    changed.notify_all();
    worker.join();
}
void CoreQueueSink::enqueue(std::function<void()> run, double seconds) {
    std::lock_guard<std::mutex> lock(mutex);
    if (closing || !workerFault.empty())
        throw std::runtime_error("queue worker unavailable: " + workerFault);
    if (jobs.size() >= 8192)
        throw std::runtime_error("queue control job limit");
    staged += seconds;
    jobs.push_back({seconds, std::move(run)});
    changed.notify_all();
}
void CoreQueueSink::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return closing || !jobs.empty(); });
            if (jobs.empty() && closing)
                break;
            job = std::move(jobs.front());
            jobs.pop_front();
            busy = true;
        }
        try {
            job.run();
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(mutex);
            workerFault = e.what();
            const bool storage = dynamic_cast<const LengthFault *>(&e) == nullptr;
            for (auto &p : records)
                if (p.second.item) {
                    p.second.fault = e.what();
                    p.second.storageFault = p.second.storageFault || storage;
                    p.second.item->cancel(e.what());
                }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            staged -= job.seconds;
            busy = false;
            changed.notify_all();
        }
    }
}
void CoreQueueSink::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return jobs.empty() && !busy; });
    if (!workerFault.empty())
        throw std::runtime_error(workerFault);
}
std::vector<QueueRecord> CoreQueueSink::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<QueueRecord> out;
    for (const auto &p : records)
        out.push_back(p.second);
    return out;
}
void CoreQueueSink::coreEvent(const std::string &event, uint64_t id, uint64_t) {
    if (event == "strm")
        latestProducer = id;
    const double stamp = now();
    const auto receivedAt = std::chrono::steady_clock::now();
    enqueue([=] {
        std::lock_guard<std::mutex> lock(mutex);
        auto &r = records[id];
        r.source.occurrence = id;
        if (event == "strm") {
            r.strm = stamp;
            r.strmAt = receivedAt;
            r.expectedIndex = nextIndex++;
        } else if (event.rfind("codec:", 0) == 0 && event.size() == 7)
            r.source.codec = event[6];
        else if (event == "decode-complete")
            r.decoded = stamp;
        else if (event == "STMd")
            r.stmd = stamp;
        else if (event.rfind("fault:", 0) == 0)
            r.fault = event.substr(6);
        else if (event == "STMs")
            r.stms = stamp;
        else if (event == "queue-refetch") {
            r.incomplete = "superseded by queue seek";
            descriptorOrder.erase(std::remove(descriptorOrder.begin(), descriptorOrder.end(), id),
                                  descriptorOrder.end());
            if (r.item)
                r.item->cancel("queue seek superseded incomplete production; LMS re-render required");
        }
    });
}
void CoreQueueSink::protectItems() {
    std::shared_ptr<Item> current, next;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto id = audibleId ? audibleId : active;
        const auto owner = id > UINT64_MAX / 2 ? UINT64_MAX - id : id;
        auto it = records.find(id);
        if (it != records.end())
            current = it->second.item;
        for (const auto &p : records)
            if (p.first > owner && p.first < UINT64_MAX / 2 && p.second.item) {
                next = p.second.item;
                break;
            }
    }
    store.protect(current, next);
}
void CoreQueueSink::commit(uint64_t previous, const yeney::TrackDescriptor &next) {
    QueueRecord old;
    {
        std::lock_guard<std::mutex> lock(mutex);
        old = records[previous];
    }
    if (!old.item || old.promised || !old.source.sourceFrames)
        return;
    uint64_t overlap = 0;
    if (next.transition == 1 && next.rate == old.source.rate) {
        if (!next.sourceFrames)
            return; // no speculative short-track prediction
        overlap = std::min({next.transitionFrames,
                            *old.source.sourceFrames ? *old.source.sourceFrames - 1 : 0, *next.sourceFrames});
    }
    auto promise = *old.source.sourceFrames - overlap + old.prefixFrames;
    if (old.item->producedFrames() > promise)
        return; // already sealed/late: preserve audio
    auto item = store.publish(old.item, promise);
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto &r = records[previous];
        r.item = item;
        r.promised = promise;
        r.publication = "progressive";
    }
}
void CoreQueueSink::trackDescriptor(const yeney::TrackDescriptor &d) {
    enqueue([=] {
        uint64_t previous = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (describedLast != d.occurrence) {
                previous = describedLast;
                descriptorOrder.push_back(d.occurrence);
                describedLast = d.occurrence;
            }
            records[d.occurrence].source = d;
        }
        if (previous)
            commit(previous, d);
        // Current strm carries player-level settings: promise the outgoing
        // transition before a successor is requested. Unknown counts stay sealed.
        commit(d.occurrence, d);
    });
}
void CoreQueueSink::trackBoundary(uint64_t frame, const yeney::Format &f, bool) {
    // These are producer coordinates, never audible notifications.
    rate = f.rate;
    enqueue([=] {
        uint64_t id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (descriptorOrder.empty())
                throw std::runtime_error("boundary without descriptor");
            id = descriptorOrder.front();
            descriptorOrder.pop_front();
            active = id;
            records[id].first = frame;
        }
        QueueRecord tail;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = records.find(UINT64_MAX - lastEnded);
            if (it != records.end())
                tail = it->second;
        }
        auto item = store.create({std::to_string(id), f.rate, std::nullopt, 0});
        bool tailPlaying;
        {
            std::lock_guard<std::mutex> lock(mutex);
            tailPlaying = tail.item && audibleId == tail.source.occurrence;
        }
        if (freshRender && tail.item) {
            tail.item->cancel("independent LMS render excludes predecessor tail");
            std::lock_guard<std::mutex> lock(mutex);
            records.erase(UINT64_MAX - lastEnded);
        }
        if (!freshRender && tail.item && tail.source.rate == f.rate && !tailPlaying) {
            const auto count = tail.item->producedFrames();
            auto reader = tail.item->reader(44);
            char bytes[6];
            for (uint64_t i = 0; i < count; ++i) {
                size_t got = 0;
                while (got < 6)
                    got += reader.read(bytes + got, 6 - got);
                auto sample = [&](unsigned at) {
                    return int32_t(uint32_t(uint8_t(bytes[at])) << 8 |
                                   uint32_t(uint8_t(bytes[at + 1])) << 16 |
                                   uint32_t(uint8_t(bytes[at + 2])) << 24);
                };
                yeney::Frame pcm{sample(0), sample(3)};
                item->append(&pcm, 1, false);
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto &r = records[id];
                r.first = tail.first;
                r.prefixFrames = count;
                r.actual = count;
                records.erase(UINT64_MAX - lastEnded);
            }
            tail.item->cancel("tail carried into same-rate successor");
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            records[id].item = item;
        }
        freshRender = false;
        std::optional<yeney::TrackDescriptor> successor;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!descriptorOrder.empty())
                successor = records.at(descriptorOrder.front()).source;
        }
        yeney::TrackDescriptor prediction;
        {
            std::lock_guard<std::mutex> lock(mutex);
            prediction = records.at(id).source;
        }
        commit(id, prediction);
        if (successor)
            commit(id, *successor);
        protectItems();
    });
}
size_t CoreQueueSink::write(const yeney::Frame *p, size_t n, bool processed) {
    // Admission and enqueue are atomic; event-loop thread never waits for
    // storage.
    std::lock_guard<std::mutex> lock(mutex);
    if (closing || !workerFault.empty())
        return 0;
    n = std::min(n, size_t(std::max(0.0, budget - staged) * rate));
    if (!n || jobs.size() >= 8192)
        return 0;
    std::vector<yeney::Frame> data(p, p + n);
    const double seconds = double(n) / rate;
    staged += seconds;
    jobs.push_back({seconds, [this, data = std::move(data), processed] { append(data, processed); }});
    submitted += n;
    changed.notify_all();
    return n;
}
void CoreQueueSink::append(const std::vector<yeney::Frame> &pcm, bool processed) {
    QueueRecord r;
    {
        std::lock_guard<std::mutex> lock(mutex);
        r = records.at(active);
    }
    if (!r.item)
        throw std::runtime_error("PCM without item");
    auto produced = r.item->producedFrames();
    size_t n = pcm.size();
    if (r.promised)
        n = std::min<uint64_t>(n, *r.promised - produced);
    if (n)
        r.item->append(pcm.data(), n, processed);
    if (n < pcm.size()) {
        // Promise wins; retain the residual at its original rate as a continuation.
        const auto token = std::to_string(active) + "-tail";
        std::shared_ptr<Item> tail;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto &t = records[UINT64_MAX - active];
            tail = t.item;
            if (!tail) {
                t.source = r.source;
                t.expectedIndex = r.expectedIndex;
                t.source.sourceFrames.reset();
                t.source.occurrence = UINT64_MAX - active;
                t.first = r.first + *r.promised;
                t.publication = "sealed-tail";
            }
        }
        if (!tail) {
            tail = store.create({token, r.source.rate, std::nullopt, 0});
            std::lock_guard<std::mutex> lock(mutex);
            records[UINT64_MAX - active].item = tail;
        }
        tail->append(pcm.data() + n, pcm.size() - n, processed);
        {
            std::lock_guard<std::mutex> lock(mutex);
            records[UINT64_MAX - active].actual += pcm.size() - n;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        records[active].actual += n;
    }
}
void CoreQueueSink::finish(const yeney::ItemBoundary &b) {
    QueueRecord r, tail;
    {
        std::lock_guard<std::mutex> lock(mutex);
        r = records.at(b.occurrence);
        auto it = records.find(UINT64_MAX - b.occurrence);
        if (it != records.end())
            tail = it->second;
    }
    if (r.item)
        r.item->finish();
    if (tail.item)
        tail.item->finish();
    {
        std::lock_guard<std::mutex> lock(mutex);
        lastEnded = b.occurrence;
        if (!r.item)
            descriptorOrder.erase(std::remove(descriptorOrder.begin(), descriptorOrder.end(), b.occurrence),
                                  descriptorOrder.end());
        auto &v = records[b.occurrence];
        v.overlap = b.overlap;
        v.transition = b.reason;
        if (v.publication == "pending")
            v.publication = "sealed";
    }
}
void CoreQueueSink::resolvedItemBoundary(const yeney::ItemBoundary &b) {
    enqueue([=] { finish(b); });
}
void CoreQueueSink::audibleFrom(uint64_t id, uint64_t frame) {
    std::unique_lock<std::mutex> lock(mutex);
    auto it = records.find(id);
    if (it == records.end() || it->second.first != frame || !it->second.item)
        throw std::invalid_argument("invalid audible occurrence");
    if (audibleId == id)
        return;
    if (audibleId && records[audibleId].end < 0)
        records[audibleId].end = now();
    audibleId = id;
    audibleFrame = frame;
    // A selected item is not audible until at least one of its source frames
    // has actually advanced. This also fences zero-frame fault replacements.
    prefixPending = true;
    lock.unlock();
    enqueue([this] { protectItems(); });
}
void CoreQueueSink::restartCached() {
    std::lock_guard<std::mutex> lock(mutex);
    audibleId = 0;
    speakerElapsed.reset();
    speakerState.reset();
    audibleSignals.clear();
    speakerStates.clear();
}
void CoreQueueSink::beginRender() {
    refetchRequested = true;
    enqueue([this] { freshRender = true; });
}
void CoreQueueSink::onQueueCommand(std::function<bool(char, uint32_t)> handler) {
    commandHandler = std::move(handler);
}
bool CoreQueueSink::queueCommand(char command, uint32_t value) {
    return commandHandler && commandHandler(command, value);
}
void CoreQueueSink::reanchorSpeaker(uint64_t id, uint32_t origin) {
    std::lock_guard<std::mutex> lock(mutex);
    speakerElapsed.reset();
    records.at(id).sourceOriginMs = origin;
}
void CoreQueueSink::useSpeakerClock() {
    std::lock_guard<std::mutex> lock(mutex);
    speakerClock = true;
}
bool CoreQueueSink::queueItemProduced(uint64_t id) const {
    std::shared_ptr<Item> item;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = records.find(id);
        if (it != records.end())
            item = it->second.item;
    }
    if (!item)
        return false;
    const auto length = item->contentLength();
    return length && item->producedFrames() >= (*length - 44) / 6;
}
bool CoreQueueSink::speakerDriven() const {
    std::lock_guard<std::mutex> lock(mutex);
    return speakerClock;
}
void CoreQueueSink::speakerPosition(uint64_t id, uint64_t frame, yeney::PlaybackState state) {
    std::unique_lock<std::mutex> lock(mutex);
    const auto it = records.find(id);
    if (!speakerClock || it == records.end() || !it->second.item)
        return;
    auto &r = it->second;
    // A confirmed start still needs actual source PCM, not silence-only output.
    const auto produced = r.item->producedFrames();
    if (produced <= r.prefixFrames)
        return;
    const bool changed = audibleId != id;
    if (changed && state != yeney::PlaybackState::Playing)
        return;
    if (changed) {
        audibleId = id;
        audibleFrame = r.first;
        speakerState.reset();
        prefixPending = true;
    }
    frame = std::min(frame, r.promised.value_or(produced));
    audibleFrame = std::max(audibleFrame, std::min(r.first + frame, submitted));
    if (frame >= r.prefixFrames) {
        if (prefixPending) {
            prefixPending = false;
            r.audible = now();
            const auto source = id < UINT64_MAX / 2 ? id : UINT64_MAX - id;
            if (source != audibleSource) {
                if (audibleSource && records[audibleSource].end < 0)
                    records[audibleSource].end = now();
                speakerElapsed = 0;
            }
            audibleSource = source;
            if (id < UINT64_MAX / 2)
                audibleSignals.push_back({id, r.first + r.prefixFrames, true});
        }
        const auto sourceFrames =
            id < UINT64_MAX / 2 ? frame - r.prefixFrames : audibleFrame - records.at(audibleSource).first;
        const auto elapsed = uint32_t(
            std::min<uint64_t>(UINT32_MAX, uint64_t(r.sourceOriginMs) + sourceFrames * 1000 / r.source.rate));
        speakerElapsed = std::max(speakerElapsed.value_or(0), elapsed);
    } else if (audibleSource) {
        const auto &old = records.at(audibleSource);
        speakerElapsed =
            uint32_t(std::min<uint64_t>(UINT32_MAX, (audibleFrame - old.first) * 1000 / old.source.rate));
    }
    if (audibleSource && speakerState != state) {
        speakerState = state;
        speakerStates.push_back({audibleSource, state});
    }
    paused = state != yeney::PlaybackState::Playing;
    lock.unlock();
    if (changed)
        enqueue([this] { protectItems(); });
}
bool CoreQueueSink::takeAudibleState(yeney::AudibleState &state) {
    std::lock_guard<std::mutex> lock(mutex);
    if (speakerStates.empty())
        return false;
    state = speakerStates.front();
    speakerStates.pop_front();
    return true;
}
std::optional<uint32_t> CoreQueueSink::audibleElapsedMs(uint64_t occurrence) const {
    std::lock_guard<std::mutex> lock(mutex);
    return occurrence == audibleSource ? speakerElapsed : std::nullopt;
}
bool CoreQueueSink::isPaused() const {
    std::lock_guard<std::mutex> lock(mutex);
    return paused;
}
void CoreQueueSink::audibleEnd(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    records.at(id).end = now();
}
void CoreQueueSink::recordLms(uint64_t id, QueueRecord::LmsSample sample) {
    std::lock_guard<std::mutex> lock(mutex);
    auto &r = records.at(id);
    if (sample.matches && !*sample.matches) {
        r.mismatchedSeconds += clockSpeed;
        if (mismatchSince < 0)
            mismatchSince = sample.stamp;
        r.mismatchRun = sample.stamp - mismatchSince;
        r.lmsMismatch = r.lmsMismatch || r.mismatchRun > 2;
    } else {
        r.mismatchRun = 0;
        mismatchSince = -1;
    }
    r.lms.push_back(std::move(sample));
}
void CoreQueueSink::advanceAudible(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mutex);
    audibleFrame = std::max(audibleFrame, std::min(frame, submitted));
    if (prefixPending) {
        auto &r = records.at(audibleId);
        if (audibleFrame > r.first + r.prefixFrames) {
            prefixPending = false;
            r.audible = now();
            audibleSource = audibleId < UINT64_MAX / 2 ? audibleId : UINT64_MAX - audibleId;
            if (audibleId < UINT64_MAX / 2)
                audibleSignals.push_back({audibleId, r.first + r.prefixFrames});
        }
    }
}
bool CoreQueueSink::awaitingPublication(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = records.find(id);
    return it != records.end() && !it->second.promised && it->second.publication == "pending";
}
std::optional<uint64_t> CoreQueueSink::committedItemFrames(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = records.find(id);
    if (it == records.end() || !it->second.promised)
        return std::nullopt;
    return *it->second.promised - it->second.prefixFrames;
}
bool CoreQueueSink::takeAudibleStart(yeney::AudibleStart &s) {
    std::lock_guard<std::mutex> lock(mutex);
    if (audibleSignals.empty())
        return false;
    s = audibleSignals.front();
    audibleSignals.pop_front();
    return true;
}
uint64_t CoreQueueSink::audibleOccurrence() const {
    std::lock_guard<std::mutex> lock(mutex);
    return audibleSource;
}
uint64_t CoreQueueSink::audibleFrames() const {
    std::lock_guard<std::mutex> lock(mutex);
    return audibleFrame;
}
void CoreQueueSink::pause() {
    std::lock_guard<std::mutex> lock(mutex);
    paused = true;
}
void CoreQueueSink::resume() {
    std::lock_guard<std::mutex> lock(mutex);
    paused = false;
}
void CoreQueueSink::stop() { pause(); }
void CoreQueueSink::flush() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        audibleSignals.clear();
        speakerStates.clear();
        speakerElapsed.reset();
        speakerState.reset();
        audibleId = 0;
        audibleSource = 0;
        prefixPending = false;
        mismatchSince = -1;
    }
    enqueue([this] {
        std::vector<std::shared_ptr<Item>> items;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto &p : records)
                if (p.second.end < 0 && p.second.incomplete.empty())
                    p.second.incomplete = "flushed";
            for (auto &p : records)
                if (p.second.item)
                    items.push_back(p.second.item);
            active = 0;
            freshRender = true;
            nextIndex = 0;
            descriptorOrder.clear();
        }
        for (auto &i : items)
            i->cancel("queue flushed");
    });
}
} // namespace queue_audio
