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
    if (!acceptSnapshot(snapshot)) return;
    if (!lmsNavigation) {
        // A published declaration does not mean the old audible anchor is
        // current. Let a fresh speaker poll adopt the selected row first.
        for (const auto &selection : selections) {
            if (selection.second.phase != Selection::Published) return;
            if (selection.first != snapshot.index) continue;
            const auto audible = std::find_if(items.begin(), items.end(), [this](const auto &entry) {
                return entry.stream == current;
            });
            if (audible == items.end()) return;
            std::lock_guard<std::mutex> sourceLock(sourceMutex);
            const auto identity = sourceTokens.find(audible->owner ? audible->owner : audible->stream);
            if (identity == sourceTokens.end() || identity->second != selection.second.token) return;
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
    // An app burst may select another row while the preceding fresh producer
    // is being paired. Fence edits around the row actually selected now.
    if (lmsNavigation) {
        const auto stamp = std::chrono::steady_clock::now();
        const auto position = actions.position().response;
        const auto evidence = speakerChanges->poll(stamp,
            {{"CurrentTrack", position.value("Track")}, {"CurrentTrackURI", position.value("TrackURI")},
             {"RelativeTimePosition", position.value("RelTime")}});
        std::lock_guard<std::mutex> sourceLock(sourceMutex);
        for (const auto &entry : items) if (entry.uri == evidence.at("CurrentTrackURI")) {
            const auto token = sourceTokens.find(entry.owner ? entry.owner : entry.stream);
            if (token != sourceTokens.end()) anchor = token->second;
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
            selections.erase(audible->index);
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
            logQueue("window target re-produced lms_index=" + std::to_string(audible->index) +
                " origin_ms=" + std::to_string(navigationItem->originMs) +
                " lms_commands=" + std::to_string(speakerNavCommands[navigationToken]));
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
            if (slot.token == anchor || std::any_of(found->second.begin(), found->second.end(),
                            [this](const auto &entry) { return entry.stream == current; })) {
                // Back makes the still-audible parent upcoming. Select the
                // target before retiring that parent's interrupted suffix.
                deferredUpcoming = true;
            } else {
                if (const auto lazy = lazyItems.find(slot.token); lazy != lazyItems.end()) retiredLazy.push_back(lazy->second);
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
    struct SpeakerMovedDuringEdit {};
    const auto originalAnchor = std::find_if(items.begin(), items.end(), [this](const auto &entry) {
        return entry.stream == current;
    });
    const auto originalUri = originalAnchor == items.end() ? std::string{} : originalAnchor->uri;
    Edits::IO io;
    io.browse = [this] {
        const auto value = coordinator.browse();
        return Edits::Snapshot{value.update, value.uris};
    };
    io.remove = [this, &originalUri](auto update, auto first, auto count) {
        const auto stamp = std::chrono::steady_clock::now();
        const auto position = actions.position().response;
        const auto evidence = speakerChanges->poll(stamp,
            {{"CurrentTrack", position.value("Track")}, {"CurrentTrackURI", position.value("TrackURI")},
             {"RelativeTimePosition", position.value("RelTime")}});
        const auto &uri = evidence.at("CurrentTrackURI");
        if (!uri.empty() && uri != originalUri) {
            const auto fresh = coordinator.browse();
            const auto selected = std::find(fresh.uris.begin(), fresh.uris.end(), uri);
            if (selected != fresh.uris.end()) {
                const auto ordinal = unsigned(selected - fresh.uris.begin()) + 1;
                if (ordinal >= first && ordinal < first + count) throw SpeakerMovedDuringEdit{};
            }
        }
        actions.remove(update, first, count);
    };
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
    const auto superseded = lmsNavigation && std::any_of(selectionOrder.begin(), selectionOrder.end(),
        [this](unsigned index) {
            const auto selection = selections.find(index);
            return selection != selections.end() && selection->second.token != navigationToken;
        });
    bool selectedTarget = false;
    bool overlap = false;
    try {
        if (lmsNavigation && anchor == navigationToken && !superseded) {
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
    } catch (const SpeakerMovedDuringEdit &) {
        // Earlier edits may already have committed. Rebase the worker model
        // from the actual owned queue before accepting the new row selection.
        const auto fresh = coordinator.browse();
        std::vector<Entry> rebased;
        for (const auto &uri : fresh.uris) {
            const Entry *match = nullptr;
            for (const auto &parent : entries) for (const auto &entry : parent.second)
                if (entry.uri == uri) match = &entry;
            if (!match) for (const auto &entry : items) if (entry.uri == uri) match = &entry;
            if (!match) throw std::runtime_error("queue ownership changed during navigation");
            rebased.push_back(*match);
        }
        items = std::move(rebased);
        windowDirty = true;
        nextWindowPoll = {};
        fastPoll();
        logQueue("window edit rebased reason=speaker-navigation lms_commands=0");
        return;
    }
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
        if (!superseded) {
            current = target->stream;
            if (!selectedTarget) actions.seekTrack(unsigned(target - items.begin()) + 1);
            expectedState = "PLAYING";
            actions.play();
            waitingForPlay = true;
            playAt = std::chrono::steady_clock::now();
        }
        fastPoll();
        if (!overlap) logQueue("window rebuild reason=" + std::string(snapshot.shuffle != previous.shuffle ? "reorder" : "out-of-range") + " lms_commands=0");
        unsigned history = 0;
        for (const auto &parent : wanted) {
            if (parent.token == navigationToken) break;
            ++history;
        }
        logQueue("window lms-nav from_index=" + std::to_string(navigationFrom) + " to_index=" +
            std::to_string(audible->index) + " action=" + (overlap ? "slide" : "rebuild") +
            " history=" + std::to_string(history) + " lms_commands=" +
            std::to_string(speakerNavCommands[navigationToken]) + " kind=" +
            (navigationFrom == audible->index ? (snapshot.timeMs < 500 ? "previous" : "seek") : navigationFrom + 1 == audible->index ? "next" :
             navigationFrom == audible->index + 1 ? "previous" : "jump") +
            " edits=" + std::to_string(queueEdits - navigationEdits));
        if (reused) logQueue("window retained target audio reused lms_commands=0");
        if (bound) logQueue("window lazy target producer adopted lms_commands=0");
        selections.erase(audible->index);
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
            std::to_string(history) + " lms_commands=" + std::to_string(speakerNavCommands[audible->token]) + " kind=" +
            (speakerNavCommands[audible->token] ?
                (*publishedWindowIndex + 1 == audible->index ? "next" :
                 *publishedWindowIndex == audible->index + 1 ? "previous" : "jump") : "natural"));
    }
    speakerNavCommands.erase(audible->token);
    publishedWindowIndex = audible->index;
    playlist = std::move(snapshot);
    publishedWindowPolicy = window.policy();
    refreshUpcoming = deferredUpcoming;
    windowDirty = deferredUpcoming;
    if (deferredUpcoming) nextWindowPoll = {};
    publish();
}
bool QueueRoom::Impl::acceptSnapshot(queue_audio::PlaylistSnapshot &snapshot) {
    const auto known = playlist.entries.size();
    const auto busy = [this] {
        std::lock_guard<std::mutex> lock(mutex);
        return selecting || lmsNavigation || navigationSelecting || !selections.empty() ||
            selectionDeferred.load() || !commands.empty() || windowDirty ||
            navigationDeadline != std::chrono::steady_clock::time_point{} ||
            std::chrono::steady_clock::now() < ownUntil;
    };
    if (snapshot.entries.size() < known && busy()) {
        // A confirmed disjoint replacement is a new playlist generation, not
        // a truncated read of the playlist whose navigation is settling.
        if (!snapshot.entries.empty() && std::none_of(snapshot.entries.begin(), snapshot.entries.end(),
            [this](const auto &track) {
                return std::any_of(playlist.entries.begin(), playlist.entries.end(),
                    [&track](const auto &old) { return old.url == track.url; });
            })) {
            auto fresh = reader.read(host, mac);
            if (fresh.revision == snapshot.revision && fresh.index == snapshot.index &&
                fresh.entries.size() == snapshot.entries.size() &&
                std::equal(fresh.entries.begin(), fresh.entries.end(), snapshot.entries.begin(),
                    [](const auto &a, const auto &b) { return a.url == b.url; })) {
                snapshot = std::move(fresh);
                logQueue("window snapshot accepted reason=new-playlist-generation");
                return true;
            }
        }
        logQueue("window snapshot rejected reason=short-in-flight known=" + std::to_string(known) +
                 " received=" + std::to_string(snapshot.entries.size()));
        return false;
    }
    if (snapshot.entries.empty() && known) {
        auto fresh = reader.read(host, mac);
        if (!fresh.entries.empty() || busy()) {
            logQueue("window snapshot rejected reason=unconfirmed-clear");
            return false;
        }
        snapshot = std::move(fresh);
    }
    return true;
}
void QueueRoom::Impl::arbitrateSelection(uint64_t token, const std::string &reason) {
    const auto slot = window.find(token);
    if (!slot) return;
    const auto existing = selections.find(slot->index);
    if (existing != selections.end() && existing->second.token == token) {
        logQueue("selection bind lms_index=" + std::to_string(slot->index) + " reason=owned-selection lms_commands=0");
        return;
    }
    auto snapshot = reader.read(host, mac);
    if (snapshot.entries.size() < playlist.entries.size()) {
        logQueue("window snapshot rejected reason=selection-short");
        snapshot = reader.read(host, mac);
    }
    if (snapshot.entries.size() >= playlist.entries.size() && snapshot.index < snapshot.entries.size() &&
        snapshot.index == slot->index && snapshot.mode != "stop") {
        const auto lazy = lazyItems.find(token);
        if (lazy != lazyItems.end() && !lazy->second->ready()) {
            auto first = queue.latestOccurrence() + 1;
            std::lock_guard<std::mutex> sourceLock(sourceMutex);
            for (const auto &record : queue.snapshot()) {
                const auto identity = sourceTokens.find(record.source.occurrence);
                if (!record.parent && record.incomplete.empty() && record.fault.empty() &&
                    identity != sourceTokens.end() && identity->second == token)
                    first = std::min(first, record.source.occurrence);
            }
            selections[slot->index] = Selection{Selection::Producing, token, first};
        }
        logQueue("selection bind lms_index=" + std::to_string(slot->index) + " reason=already-selected lms_commands=0");
        return;
    }
    selections[slot->index] = Selection{Selection::Queued, token, 0};
    selectionOrder.push_back(slot->index);
    logQueue("selection queued lms_index=" + std::to_string(slot->index) + " reason=" + reason);
    advanceSelections();
}
void QueueRoom::Impl::selectionPublished(unsigned index) {
    const auto selection = selections.find(index);
    if (selection == selections.end() || selection->second.phase != Selection::Producing) return;
    if (selection->second.observed) {
        const auto confirmed = selection->second.first;
        for (auto old = selections.begin(); old != selections.end();) {
            if (old->first == index || (old->second.phase == Selection::Published && old->second.first <= confirmed))
                old = selections.erase(old);
            else ++old;
        }
    } else selection->second.phase = Selection::Published;
    fastPoll();
}
void QueueRoom::Impl::advanceSelections() {
    // LMS has one producer. Drain each live declaration before selecting the
    // next index, so a rapid burst cannot cancel an earlier HTTP promise.
    for (auto selection = selections.begin(); selection != selections.end();) {
        const auto lazy = lazyItems.find(selection->second.token);
        if (selection->second.phase != Selection::Published && lazy != lazyItems.end() && lazy->second->abandoned()) {
            logQueue("selection retired reason=reader-left lms_index=" + std::to_string(selection->first));
            selection = selections.erase(selection);
        } else ++selection;
    }
    for (const auto &selection : selections)
        if (selection.second.phase == Selection::Producing) return;
    while (!selectionOrder.empty()) {
        const auto index = selectionOrder.front();
        selectionOrder.pop_front();
        const auto selection = selections.find(index);
        if (selection == selections.end()) continue;
        selection->second.phase = Selection::Producing;
        selection->second.first = queue.latestOccurrence() + 1;
        speakerNavFirst[selection->second.token] = selection->second.first;
        roomLmsCommand(host, mac, "playlist index " + std::to_string(index),
                       lazyItems.count(selection->second.token) && lazyItems.at(selection->second.token)->waiting() ?
                           "lazy-pcm-get" : "speaker-nav", logQueue);
        if (const auto lazy = lazyItems.find(selection->second.token); lazy != lazyItems.end()) {
            lazy->second->commandIssued();
            logQueue("lazy activated lms_index=" + std::to_string(index) + " lms_commands=1");
        }
        speakerNavCommands[selection->second.token] = 1;
        logQueue("selection issued lms_index=" + std::to_string(index) + " lms_commands=1");
        return;
    }
}

void QueueRoom::Impl::activateLazy(uint64_t token) {
    append();
    const auto found = lazyItems.find(token);
    if (found == lazyItems.end() || !window.find(token) || !found->second->claimActivation()) return;
    arbitrateSelection(token, "lazy-pcm-get");
}
} // namespace bridge
