// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace queue_audio {
double CoreQueueSink::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count() * clockSpeed;
}
CoreQueueSink::CoreQueueSink(const std::string &room, Config c, double seconds, double speed)
    : store(room, c), continuationLimit(std::min<uint64_t>((UINT32_MAX-36ull)/6,
          std::max(c.diskBytes,c.estimatedDiskBytes)/6)), budget(seconds), clockSpeed(speed) {
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
        } catch (const ItemCancelled &) {
            // A queued job may still reference an item retired by LMS flush.
            // Drop that job; cancellation must not poison the next producer.
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
    std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return std::make_pair(a.parent ? a.parent : a.source.occurrence, a.segmentOffset) <
               std::make_pair(b.parent ? b.parent : b.source.occurrence, b.segmentOffset);
    });
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
        else if (event.rfind("fault:", 0) == 0) {
            r.fault = event.substr(6);
            r.storageFault = r.storageFault || r.fault.rfind("encoded spool",0) == 0 || r.fault.rfind("queue disk quota",0) == 0;
        }
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
void CoreQueueSink::windowRetention(unsigned history, unsigned upcoming) {
    std::lock_guard<std::mutex> lock(mutex);
    retainedHistory = history;
    retainedUpcoming = upcoming;
}
void CoreQueueSink::releaseParent(uint64_t owner) {
    enqueue([this, owner] {
        std::vector<std::shared_ptr<Item>> released;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto &p : records) {
                if (p.first != owner && p.second.parent != owner) continue;
                p.second.incomplete = "outside queue window";
                if (p.second.item) released.push_back(p.second.item);
            }
        }
        for (auto &item : released) item->releaseAudio();
        protectItems();
    });
}
void CoreQueueSink::protectItems() {
    std::vector<std::shared_ptr<Item>> retained;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto id = audibleId ? audibleId : active;
        const auto audible = records.find(id);
        const auto owner = audible != records.end() && audible->second.parent ? audible->second.parent :
                           id > UINT64_MAX / 2 ? UINT64_MAX - id : id;
        std::vector<uint64_t> before, after;
        for (const auto &p : records) {
            if (p.first >= UINT64_MAX / 2 || !p.second.item || !p.second.incomplete.empty()) continue;
            (p.first <= owner ? before : after).push_back(p.first);
        }
        if (before.size() > retainedHistory + 1) before.erase(before.begin(), before.end() - retainedHistory - 1);
        if (after.size() > retainedUpcoming) after.resize(retainedUpcoming);
        before.insert(before.end(), after.begin(), after.end());
        for (const auto &p : records) {
            const auto parent = p.second.parent ? p.second.parent : p.first;
            if (std::find(before.begin(), before.end(), parent) != before.end()) retained.push_back(p.second.item);
        }
        // Temporary outgoing tails remain protected until carried or published.
        const auto tail = records.find(id);
        if (tail != records.end()) retained.push_back(tail->second.item);
    }
    store.protect(retained);
}
void CoreQueueSink::commit(uint64_t previous, const yeney::TrackDescriptor &next) {
    QueueRecord old;
    {
        std::lock_guard<std::mutex> lock(mutex);
        old = records[previous];
    }
    if (old.estimated || !old.item || old.promised || !old.source.sourceFrames)
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
void CoreQueueSink::estimate(uint64_t id, double duration, unsigned originMs) {
    // Source capability must be visible to the core before it opens input.
    std::lock_guard<std::mutex> lock(mutex);
    auto &r = records[id];
    r.estimated = true;r.duration = duration;r.sourceOriginMs = originMs;
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
        QueueRecord estimated;
        {
            std::lock_guard<std::mutex> lock(mutex);
            estimated = records.at(id);
        }
        Descriptor descriptor{std::to_string(id), f.rate, std::nullopt, 0};
        if (estimated.estimated) {
            // Remote status duration can describe the remaining stream already.
            // The pre-start player clock can still belong to the previous run.
            // Preserve the larger bound; never subtract that clock a second time.
            const auto remaining = estimated.duration;
            descriptor.promisedFrames = uint64_t(std::ceil(remaining * f.rate)) + uint64_t(remoteMarginSeconds) * f.rate;
            descriptor.kind = PromiseKind::UpperBound;
        }
        auto item = store.create(descriptor);
        if (estimated.estimated) {
            std::lock_guard<std::mutex> lock(mutex);
            records[id].promised = descriptor.promisedFrames;
            records[id].publication = "upper-bound";
            producingSegment = id;
            std::printf("yeney: queue promise occurrence=%llu lms_duration_s=%.6f lms_start_offset_ms=0 lms_status_time_ms=%u declared_frames=%llu declared_s=%.6f kind=upper-bound reason=duration-preserved-clock-not-subtracted\n",
                        (unsigned long long)id, estimated.duration, estimated.sourceOriginMs,
                        (unsigned long long)*descriptor.promisedFrames, double(*descriptor.promisedFrames)/f.rate);
        }
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
    if (r.estimated) {
        appendEstimated(pcm, processed);
        return;
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
// Only the storage worker publishes pending overflow. Unpublished PCM remains
// in the quota-accounted compressed store; no HTTP route exposes a mutable header.
void CoreQueueSink::publishEstimated(uint64_t id, bool force) {
    QueueRecord r, parent;
    double lead;
    uint64_t submittedThrough=0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        r=records.at(id);
        if(!r.parent || r.promised || !r.item || !r.incomplete.empty())return;
        parent=records.at(r.parent);
        if(active==r.parent)submittedThrough=submitted;
        lead=double(r.segmentOffset)/r.source.rate;
        if(audibleSource==r.parent && speakerElapsed)lead-=double(*speakerElapsed)/1000;
    }
    const auto produced=r.item->producedFrames();
    const bool resolved=parent.finalFrame && *parent.finalFrame>r.first;
    const double resolvedLead=double(r.segmentOffset)/r.source.rate-double(parent.finalElapsedMs.value_or(0))/1000;
    const bool exact=resolved && (resolvedLead>=10 || lead>=10);
    if(!exact && !force && lead>10)return;
    // A resolved end includes submitted PCM still awaiting the storage worker.
    const auto acceptedOverflow=resolved?*parent.finalFrame-r.first:
        submittedThrough>r.first?submittedThrough-r.first:0;
    const auto overflowProduced=std::max(produced,acceptedOverflow);
    const auto promised=exact?*parent.finalFrame-r.first:
        std::min(continuationLimit,overflowProduced+*parent.promised);
    if(!promised || promised<produced)throw std::runtime_error("continuation exceeds room/WAV bound");
    const auto kind=exact?PromiseKind::Exact:PromiseKind::UpperBound;
    auto item=store.publish(r.item,promised,kind);
    const auto decision=r.continuationDecision=="bound-exceeded"?r.continuationDecision:
        exact?"exact":"deadline-forced";
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto &next=records.at(id);
        next.item=item;next.promised=promised;next.continuationDecision=decision;
        next.publication=exact?"exact-continuation":"upper-bound-continuation";
    }
    std::printf("yeney: queue continuation published parent=%llu occurrence=%llu decision=%s kind=%s offset_frames=%llu declared_frames=%llu produced_frames=%llu spooled_frames=%llu remaining_lead_s=%.6f\n",
        (unsigned long long)r.parent,(unsigned long long)id,decision.c_str(),exact?"exact":"upper-bound",
        (unsigned long long)r.segmentOffset,(unsigned long long)promised,(unsigned long long)overflowProduced,
        (unsigned long long)produced,lead);
}
uint64_t CoreQueueSink::continuationAfter(uint64_t predecessor) {
    QueueRecord r;
    {
        std::lock_guard<std::mutex> lock(mutex);
        r=records.at(predecessor);
        const auto parent=r.parent?r.parent:predecessor;
        for(const auto &p:records)
            if(p.second.parent==parent && p.second.segmentOffset==r.segmentOffset+*r.promised)
                return p.first;
    }
    const auto id=predecessor;
        QueueRecord next = r;
        next.parent = r.parent ? r.parent : id;
        next.segmentOffset += *r.promised;
        next.first = r.first + *r.promised;
        next.actual = next.prefixFrames = 0;
        next.incomplete.clear();next.fault.clear();next.promised.reset();
        next.productionComplete = false;
        next.source.occurrence = ++continuationSerial;
        next.publication = "pending-continuation";
        next.continuationDecision=r.parent?"bound-exceeded":"pending";
        next.item = store.create({std::to_string(next.source.occurrence), r.source.rate, std::nullopt,0});
        {
            std::lock_guard<std::mutex> lock(mutex);
            records.emplace(next.source.occurrence, next);
            std::printf("yeney: queue continuation created parent=%llu occurrence=%llu offset_frames=%llu decision=%s\n",
                        (unsigned long long)next.parent, (unsigned long long)next.source.occurrence,
                        (unsigned long long)next.segmentOffset,next.continuationDecision.c_str());
        }
        protectItems();
    return next.source.occurrence;
}
void CoreQueueSink::appendEstimated(const std::vector<yeney::Frame> &pcm, bool processed) {
    size_t offset = 0;
    while (offset < pcm.size()) {
        QueueRecord r;
        uint64_t id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            id = producingSegment;
        }
        publishEstimated(id);
        {
            std::lock_guard<std::mutex> lock(mutex);
            r = records.at(id);
        }
        const auto produced = r.item->producedFrames();
        const auto limit=r.promised.value_or(continuationLimit);
        const size_t n = std::min<uint64_t>(pcm.size() - offset, limit - produced);
        if (n) r.item->append(pcm.data() + offset, n, processed);
        offset += n;
        {
            std::lock_guard<std::mutex> lock(mutex);
            records.at(id).actual += n;
        }
        if (offset == pcm.size()) break;
        if(!r.promised) {
            publishEstimated(id,true);
            std::lock_guard<std::mutex> lock(mutex);
            r=records.at(id);
            if (*r.promised > produced + n) continue; // end resolved during the pending append
        }
        std::printf("yeney: queue continuation overflow parent=%llu produced_frames=%llu declared_frames=%llu\n",
                    (unsigned long long)(r.parent ? r.parent : id),
                    (unsigned long long)(*r.promised + pcm.size() - offset), (unsigned long long)*r.promised);
        r.item->finish();
        const auto next=continuationAfter(id);
        {
            std::lock_guard<std::mutex> lock(mutex);
            producingSegment=next;
        }
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
    if (r.estimated) {
        uint64_t pending=0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for(const auto &p:records)if(p.second.parent==b.occurrence && !p.second.promised)pending=p.first;
        }
        if(pending)publishEstimated(pending);
        std::vector<std::shared_ptr<Item>> segments;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto &p : records)
                if (p.first == b.occurrence || p.second.parent == b.occurrence) {
                    p.second.productionComplete = true;
                    segments.push_back(p.second.item);
                }
            lastEnded = b.occurrence;
        }
        for (const auto &item : segments) if (item) item->finish();
        return;
    }
    if (r.item) {
        if (!r.fault.empty() && r.promised && r.item->producedFrames() != *r.promised)
            r.item->cancel(r.fault); // An unfulfillable promise is not a storage-worker failure.
        else
            r.item->finish();
    }
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
    {
        // The end is authoritative once all source PCM has been submitted.
        // Publish that fact before queued storage jobs create later segments;
        // an already announced segment always keeps its immutable promise.
        std::lock_guard<std::mutex> lock(mutex);
        records[b.occurrence].finalFrame = b.frame;
        records[b.occurrence].finalElapsedMs = audibleSource == b.occurrence ? speakerElapsed.value_or(0) : 0;
    }
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
    cachedReanchor = true;
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
std::optional<yeney::EncodedSpool> CoreQueueSink::encodedSpool(uint64_t id) {
    bool estimated = false;
    if (id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = records.find(id);
        estimated = it != records.end() && it->second.estimated;
    }
    return store.reserveEncoded(estimated);
}
bool CoreQueueSink::queueEndRequiresPlayed(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = records.find(id);
    return it != records.end() && it->second.estimated;
}
bool CoreQueueSink::queueItemProduced(uint64_t id) const {
    std::shared_ptr<Item> item;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = records.find(id);
        if (it != records.end() && it->second.estimated) return it->second.productionComplete;
        if (it != records.end())
            item = it->second.item;
    }
    if (!item)
        return false;
    try {
        const auto length = item->contentLength();
        return length && item->producedFrames() >= (*length - 44) / 6;
    } catch (const ItemCancelled &) {
        return false;
    }
}
bool CoreQueueSink::queuePrefixProduced(uint64_t id, uint64_t frames) const {
    std::shared_ptr<Item> item;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto i = records.find(id);
        if (i != records.end())
            item = i->second.item;
    }
    return item && item->producedFrames() >= frames;
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
    if (!produced && r.parent && r.productionComplete &&
        state==yeney::PlaybackState::Stopped && audibleSource==r.parent) {
        uint64_t total=0;
        for(const auto &p:records)
            if(p.first==r.parent || p.second.parent==r.parent)total+=p.second.actual;
        audibleId=id;
        audibleFrame=records.at(r.parent).first+total;
        speakerElapsed=uint32_t(std::min<uint64_t>(UINT32_MAX,total*1000/r.source.rate));
        if(speakerState!=state)speakerStates.push_back({r.parent,state});
        speakerState=state;paused=true;
        return;
    }
    if (produced <= r.prefixFrames)
        return;
    const bool changed = audibleId != id;
    const bool completedContinuation = state == yeney::PlaybackState::Stopped && r.parent &&
        audibleSource == r.parent && r.productionComplete;
    if (changed && state != yeney::PlaybackState::Playing && !completedContinuation)
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
            const auto source = r.parent ? r.parent : id < UINT64_MAX / 2 ? id : UINT64_MAX - id;
            if (source != audibleSource) {
                if (audibleSource && records[audibleSource].end < 0)
                    records[audibleSource].end = now();
                speakerElapsed = 0;
            }
            audibleSource = source;
            if (!r.parent && id < UINT64_MAX / 2)
                audibleSignals.push_back({id, r.first + r.prefixFrames, true});
            else if (r.parent && cachedReanchor) {
                const auto &parent = records.at(r.parent);
                audibleSignals.push_back({r.parent, parent.first + parent.prefixFrames, true});
            }
            cachedReanchor = false;
        }
        const auto sourceFrames =
            r.estimated ? r.segmentOffset + frame :
            id < UINT64_MAX / 2 ? frame - r.prefixFrames : audibleFrame - records.at(audibleSource).first;
        const auto elapsed = uint32_t(
            std::min<uint64_t>(UINT32_MAX, uint64_t(r.estimated ? 0 : r.sourceOriginMs) + sourceFrames * 1000 / r.source.rate));
        speakerElapsed = r.estimated ? elapsed : std::max(speakerElapsed.value_or(0), elapsed);
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
    const bool estimatedEpisode = r.estimated;
    lock.unlock();
    if (changed || estimatedEpisode) enqueue([this, changed, id] {
        bool deadline=false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto &playing=records.at(id);
            const auto parent=playing.parent?playing.parent:id;
            deadline=playing.estimated && !playing.parent && playing.promised && !records.at(parent).finalFrame &&
                playing.incomplete.empty() && audibleId==id && audibleSource==parent && speakerElapsed &&
                double(*playing.promised)/playing.source.rate-double(*speakerElapsed)/1000<=10;
        }
        if(deadline)publishEstimated(continuationAfter(id));
        std::vector<uint64_t> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for(const auto &p:records)
                if(p.second.parent && !p.second.promised && p.second.item && p.second.incomplete.empty())pending.push_back(p.first);
        }
        for(auto id:pending)publishEstimated(id);
        if(changed)protectItems();
    });
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
    if (it->second.estimated) {
        uint64_t total = 0;
        for (const auto &p : records)
            if (p.first == id || p.second.parent == id) total += p.second.actual;
        return total;
    }
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
        cachedReanchor = false;
        audibleSource = 0;
        prefixPending = false;
        mismatchSince = -1;
    }
    enqueue([this] {
        std::vector<std::shared_ptr<Item>> items;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto &p : records)
                if ((p.second.item || !p.second.estimated) && p.second.end < 0 && p.second.incomplete.empty())
                    p.second.incomplete = "flushed";
            for (auto &p : records)
                if (p.second.item)
                    items.push_back(p.second.item);
            active = 0;
            freshRender = true;
            nextIndex = 0;
            descriptorOrder.clear();
        }
        for (auto &i : items) {
            try {
                const auto length = i->contentLength();
                if (i->finished() || (length && i->producedFrames() == (*length - 44) / 6))
                    continue;
            } catch (const std::exception &) {
                continue;
            } // Already failed/cancelled.
            i->cancel("queue flushed");
        }
    });
}
} // namespace queue_audio
