// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::reconcileWindow() {
    if (!started || faulted || externalReleased || sourceAway || !current || selecting) return;
    if (lmsNavigation && !navigationItem) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < nextWindowPoll) return;
    nextWindowPoll = now + std::chrono::milliseconds(pollMs);
    queue_audio::PlaylistSnapshot snapshot;
    try {
        snapshot = reader.read(host, mac);
    } catch (const std::exception &) {
        // A failed read supplies no evidence of an LMS edit. Keep the last
        // confirmed window; the next ordinary worker poll may try a new read.
        logQueue("window snapshot unavailable reason=cli-read");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!commands.empty()) {
            nextWindowPoll = {};
            return; // A controller move superseded this status read.
        }
    }
    const auto previous = publishedWindowPolicy;
    window.reconcile(snapshot);
    const bool changed = windowDirty || previous.revision != snapshot.revision || previous.shuffle != snapshot.shuffle ||
                         previous.repeat != snapshot.repeat;
    if (!changed) return;
    using Edits = queue_audio::WindowEdits;
    std::vector<Edits::Parent> before, wanted;
    std::map<uint64_t, std::vector<Entry>> entries;
    uint64_t anchor = 0;
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        for (const auto &entry : items) {
            const auto owner = entry.owner ? entry.owner : entry.stream;
            const auto token = sourceTokens.find(owner);
            if (token == sourceTokens.end()) return; // No speculative identity matching.
            if (before.empty() || before.back().token != token->second)
                before.push_back({token->second, {}});
            before.back().uris.push_back(entry.uri);
            entries[token->second].push_back(entry);
            if (entry.stream == current) anchor = token->second;
            if (const auto slot = window.find(token->second)) sourceIndices[owner] = slot->index;
        }
    }
    const auto audible = lmsNavigation ? window.find(navigationToken) : window.find(anchor);
    if (!audible) {
        // Deleting/clearing the audible occurrence makes preserving its LMS
        // identity impossible. Release only our owned queue; no CLI echo.
        logQueue("window rebuild reason=" + std::string(snapshot.entries.empty() ? "clear" : "reorder") + " lms_commands=0");
        logQueue("window lms-nav from_index=" + std::to_string(publishedWindowIndex.value_or(playlist.index)) +
            " to_index=" + std::to_string(snapshot.index) + " action=rebuild history=0 lms_commands=0 kind=" +
            (snapshot.entries.empty() ? "clear" : "reorder"));
        release(true);
        queue.flush();
        firstStream = queue.latestOccurrence() + 1;
        stoppedPlay = true;
        playlist = std::move(snapshot);
        publishedWindowPolicy = window.policy();
        windowDirty = false;
        return;
    }
    bool reused = false, bound = false, deferredUpcoming = false;
    std::vector<Entry> retired;
    std::vector<std::shared_ptr<queue_audio::LazyItem>> retiredLazy;
    if (lmsNavigation) {
        auto &target = entries[navigationToken];
        // A complete retained target serves the speaker from its existing URI.
        // Seek runs have a different declaration and need a new URI.
        const auto lazy = lazyItems.find(navigationToken);
        if (target.size() == 1 && !target.front().item && lazy != lazyItems.end() &&
            lazy->second->waiting() && lazy->second->publish(navigationItem->item)) {
            // The controller supplied this producer. Keep a waiting PCM GET on
            // its existing route instead of cancelling and selecting it again.
            target.front().stream = navigationItem->stream;
            target.front().item = navigationItem->item;
            target.front().frames = navigationItem->frames;
            target.front().originMs = navigationItem->originMs;
            owned.erase(navigationItem->uri);
            if (activatingToken == navigationToken) { activatingToken = 0; lazyTarget = -1; }
            bound = true;
        } else if (target.size() == 1 && target.front().item &&
            target.front().item->descriptor().kind == queue_audio::PromiseKind::Exact &&
            target.front().item->producedFrames() == target.front().frames &&
            !target.front().originMs && target.front().frames == navigationItem->frames && snapshot.timeMs < 500) {
            if (!target.front().owner) target.front().owner = target.front().stream;
            target.front().stream = navigationItem->stream;
            owned.erase(navigationItem->uri);
            reused = true;
        } else {
            retired.insert(retired.end(), target.begin(), target.end());
            if (const auto lazy = lazyItems.find(navigationToken); lazy != lazyItems.end()) {
                retiredLazy.push_back(lazy->second);
                lazyItems.erase(lazy);
            }
            target = {*navigationItem};
        }
    }
    for (const auto &slot : window.around(audible->index)) {
        auto found = entries.find(slot.token);
        if ((lmsNavigation || refreshUpcoming) && slot.index > audible->index && found != entries.end() &&
            !found->second.empty() && found->second.front().item &&
            (found->second.front().originMs || (found->second.front().item->finished() &&
             found->second.front().item->producedFrames() < found->second.front().frames))) {
            // A seek/interrupted suffix is valid history, but cannot supply a later
            // full play of that parent from the beginning.
            if (std::any_of(found->second.begin(), found->second.end(),
                            [this](const auto &entry) { return entry.stream == current; })) {
                // Back makes the still-audible parent upcoming. Select the
                // target before retiring that parent's interrupted suffix.
                deferredUpcoming = true;
            } else {
                if (const auto lazy = lazyItems.find(slot.token); lazy != lazyItems.end()) lazy->second->cancel();
                retired.insert(retired.end(), found->second.begin(), found->second.end());
                entries.erase(found);
                found = entries.end();
            }
        }
        if (slot.index < audible->index && found != entries.end() &&
            (found->second.empty() || !found->second.front().item)) continue;
        if (found == entries.end()) {
            if (slot.index <= audible->index) continue; // History selection is a later stage.
            const auto lazy = std::make_shared<queue_audio::LazyItem>(nullptr, [this, token = slot.token] {
                std::lock_guard<std::mutex> lock(mutex);
                if (closing) return;
                commands.push_back({'L', token});
                this->changed.notify_all();
            }, true);
            upnp::QueueMetadata metadata;
            metadata.url = speaker->controllerUri() + route.publishDeferred(1, "lazy-" + std::to_string(slot.token) + "-" + std::to_string(++lazySerial),
                [lazy, token = slot.token](upnp::StreamRequest &request) {
                    const auto start = std::chrono::steady_clock::now();
                    auto item = lazy->resolve(request, std::chrono::seconds(3));
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
                    std::printf("yeney: queue lazy %s token=%llu elapsed_ms=%lld lms_commands=%u\n",
                        item ? "headers" : (ms >= 2990 ? "deadline" : "abandoned"),
                        (unsigned long long)token, (long long)ms, lazy->commandCount());
                    return item;
                }, [lazy] { return lazy->ready(); });
            metadata.session = session;
            metadata.title = slot.track.title; metadata.artist = slot.track.artist; metadata.album = slot.track.album;
            metadata.artwork = slot.track.artwork(host);
            metadata.rate = 1; // Duration scale only; deferred DIDL omits sampleFrequency.
            metadata.frames = uint64_t(slot.track.duration);
            metadata.deferred = true;
            upnp::sanitizeXmlFields({{"title", &metadata.title}, {"artist", &metadata.artist},
                {"album", &metadata.album}, {"artwork", &metadata.artwork}});
            // Placeholder identities descend from the top; core continuations grow
            // upward from the midpoint. Never alias their first continuation IDs.
            const auto id = UINT64_MAX - slot.token;
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                sourceTokens[id] = slot.token;
                sourceIndices[id] = slot.index;
            }
            lazyItems[slot.token] = lazy;
            entries[slot.token].push_back({id, metadata.url, {}, slot.track.url, 0, metadata.title, 0, 0, upnp::queueDidl(metadata)});
            owned.insert(metadata.url);
            size_t ordinal = 1;
            for (const auto &parent : wanted) ordinal += parent.uris.size();
            logQueue("lazy declared ordinal=" + std::to_string(ordinal) + " lms_index=" + std::to_string(slot.index) +
                " kind=" + (remotePlaybackSource(slot.track.id, slot.track.remote, slot.track.url) ? "upper-bound" : "deferred"));
        }
        Edits::Parent parent{slot.token, {}};
        for (const auto &entry : entries.at(slot.token)) parent.uris.push_back(entry.uri);
        wanted.push_back(std::move(parent));
    }
    // Live/unknown duration is outside the finite window model. Preserve the
    // established natural producer path rather than discard its audible URI.
    if (wanted.empty()) return;
    Edits::IO io;
    io.browse = [this] {
        const auto value = coordinator.browse();
        return Edits::Snapshot{value.update, value.uris};
    };
    io.remove = [this](auto update, auto first, auto count) { actions.remove(update, first, count); };
    io.insert = [this, &entries](auto token, auto segment, auto ordinal) {
        const auto &entry = entries.at(token).at(segment);
        const auto reply = actions.add(entry.uri, entry.didl, ordinal);
        if (reply.response.value("NumTracksAdded") != "1")
            throw std::runtime_error("queue-window-enqueue-unconfirmed");
    };
    io.log = [&snapshot, &before, this](const auto &message) {
        std::string indices;
        for (const auto &parent : before) {
            const auto slot = window.find(parent.token);
            indices += (indices.empty() ? "" : ",") + (slot ? std::to_string(slot->index) : "removed");
        }
        logQueue(message + " lms_indices=" + indices + " reason=" +
                 (lmsNavigation || refreshUpcoming ? "lms-navigation" : "playlist-change") +
                 " shuffle=" + std::to_string(snapshot.shuffle) + " repeat=" + std::to_string(snapshot.repeat));
    };
    std::optional<uint64_t> editAnchor = anchor;
    bool selectedTarget = false;
    if (lmsNavigation && anchor == navigationToken) {
        auto old = std::find_if(before.begin(), before.end(), [this](const auto &parent) {
            return parent.token == navigationToken;
        });
        const auto target = std::find_if(wanted.begin(), wanted.end(), [this](const auto &parent) {
            return parent.token == navigationToken;
        });
        if (old != before.end() && target != wanted.end() && old->uris != target->uris) {
            // Restarting an incomplete current parent needs a new declaration.
            // Insert and select it before removing the old audible URI.
            unsigned ordinal = 1;
            std::vector<std::string> expected;
            for (auto it = before.begin(); it != before.end(); ++it) {
                if (it < old) ordinal += it->uris.size();
                expected.insert(expected.end(), it->uris.begin(), it->uris.end());
            }
            auto check = [&] {
                auto value = io.browse();
                if (value.uris != expected) throw std::runtime_error("queue window ownership changed");
                return value;
            };
            auto update = check().update;
            for (unsigned segment = 0; segment < target->uris.size(); ++segment) {
                if (check().update != update) throw std::runtime_error("queue window revision changed");
                io.insert(target->token, segment, ordinal + segment);
                expected.insert(expected.begin() + ordinal - 1 + segment, target->uris[segment]);
                const auto after = check();
                if (after.update == update) throw std::runtime_error("queue window insertion unconfirmed");
                update = after.update;
            }
            actions.seekTrack(ordinal);
            selectedTarget = true;
            const auto latest = check();
            io.remove(latest.update, ordinal + target->uris.size(), old->uris.size());
            expected.erase(expected.begin() + ordinal - 1 + target->uris.size(),
                           expected.begin() + ordinal - 1 + target->uris.size() + old->uris.size());
            if (check().update == latest.update) throw std::runtime_error("queue window removal unconfirmed");
            io.log("queue window replace ordinal=" + std::to_string(ordinal));
            old->uris = target->uris;
        }
    }
    bool overlap = false;
    if (lmsNavigation) {
        editAnchor.reset();
        for (const auto &old : before) for (const auto &next : wanted) {
            if (old.token != next.token) continue;
            if (old.uris == next.uris) {
                overlap = true;
                editAnchor = old.token;
            }
        }
    }
    Edits(std::move(io)).apply(before, wanted, editAnchor);
    for (const auto &lazy : retiredLazy) lazy->cancel();
    for (const auto &entry : retired) {
        const auto owner = entry.owner ? entry.owner : entry.stream;
        queue.releaseParent(owner);
        uint64_t token;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            token = sourceTokens.at(owner);
        }
        releaseWindowParent(token, navigationFirst);
        owned.erase(entry.uri);
    }
    std::vector<Entry> reordered;
    std::set<uint64_t> retained;
    for (const auto &parent : wanted) {
        retained.insert(parent.token);
        for (const auto &entry : entries.at(parent.token)) reordered.push_back(entry);
    }
    for (const auto &parent : before) {
        if (retained.count(parent.token)) continue;
        const auto &entry = entries.at(parent.token).front();
        if (const auto lazy = lazyItems.find(parent.token); lazy != lazyItems.end()) lazy->second->cancel();
        queue.releaseParent(entry.owner ? entry.owner : entry.stream);
        releaseWindowParent(parent.token);
        // A cached target may have a newer LMS producer clock while serving
        // its retained older audio. Retire both when the parent leaves.
        if (entry.owner && entry.owner != entry.stream) queue.releaseParent(entry.stream);
        for (const auto &uri : parent.uris) owned.erase(uri);
    }
    items.swap(reordered);
    if (lmsNavigation) {
        const auto target = std::find_if(items.begin(), items.end(), [&](const auto &entry) {
            return entry.stream == navigationItem->stream;
        });
        if (target == items.end()) throw std::logic_error("navigation target missing");
        current = target->stream;
        if (!selectedTarget) actions.seekTrack(unsigned(target - items.begin()) + 1);
        expectedState = "PLAYING";
        actions.play();
        waitingForPlay = true;
        playAt = std::chrono::steady_clock::now();
        fastPoll();
        if (!overlap) logQueue("window rebuild reason=" + std::string(snapshot.shuffle != previous.shuffle ? "reorder" : "out-of-range") + " lms_commands=0");
        unsigned history = 0;
        for (const auto &parent : wanted) {
            if (parent.token == navigationToken) break;
            ++history;
        }
        logQueue("window lms-nav from_index=" + std::to_string(navigationFrom) + " to_index=" +
            std::to_string(audible->index) + " action=" + (overlap ? "slide" : "rebuild") +
            " history=" + std::to_string(history) + " lms_commands=0 kind=" +
            (navigationFrom == audible->index ? (snapshot.timeMs < 500 ? "previous" : "seek") : navigationFrom + 1 == audible->index ? "next" :
             navigationFrom == audible->index + 1 ? "previous" : "jump") +
            " edits=" + std::to_string(queueEdits - navigationEdits));
        if (reused) logQueue("window retained target audio reused lms_commands=0");
        if (bound) logQueue("window lazy target producer adopted lms_commands=0");
        lmsNavigation = false;
        navigationItem.reset();
    } else if (publishedWindowIndex && *publishedWindowIndex != audible->index) {
        unsigned history = 0;
        for (const auto &parent : wanted) {
            if (parent.token == audible->token) break;
            ++history;
        }
        logQueue("window lms-nav from_index=" + std::to_string(*publishedWindowIndex) +
            " to_index=" + std::to_string(audible->index) + " action=slide history=" +
            std::to_string(history) + " lms_commands=0 kind=natural");
    }
    publishedWindowIndex = audible->index;
    playlist = std::move(snapshot);
    publishedWindowPolicy = window.policy();
    refreshUpcoming = deferredUpcoming;
    windowDirty = deferredUpcoming;
    if (deferredUpcoming) nextWindowPoll = {};
    publish();
}
void QueueRoom::Impl::activateLazy(uint64_t token) {
    // A naturally arriving stream owns its declaration even while count/rate
    // discovery is pending. Its HTTP reader waits, without selecting it again.
    append();
    if (lmsNavigation) return; // The controller's paired producer owns this selection.
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        for (const auto &record : queue.snapshot()) {
            const auto identity = sourceTokens.find(record.source.occurrence);
            if (!record.parent && identity != sourceTokens.end() && identity->second == token &&
                record.incomplete.empty() && record.fault.empty()) return;
        }
    }
    auto found = lazyItems.find(token);
    const auto slot = window.find(token);
    if (found == lazyItems.end() || !slot || activatingToken || !found->second->claimActivation()) return;
    activatingToken = token;
    lazyTarget = int(slot->index);
    try {
        roomLmsCommand(host, mac, "playlist index " + std::to_string(slot->index), "lazy-pcm-get", logQueue);
        logQueue("lazy activated lms_index=" + std::to_string(slot->index) + " lms_commands=1");
    } catch (...) {
        found->second->cancel(); lazyTarget = -1; activatingToken = 0;
        logQueue("lazy abandoned reason=activation-failed lms_commands=1");
    }
}
} // namespace bridge
