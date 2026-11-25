// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
#include "wav_writer.h"
namespace bridge {
void QueueRoom::Impl::reconcileWindow() {
    const auto &items = rowOwner.layout();
    const auto navigationToken = rowOwner.navigationToken();
    const auto &navigationItem = rowOwner.navigationTarget();
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
        for (const auto &selection : rowOwner.selectionSnapshot()) {
            if (selection.second.phase != Selection::Published) return;
            if (selection.first != snapshot.index) continue;
            const auto audible = std::find_if(items.begin(), items.end(), [this](const auto &entry) {
                return entry.stream == current;
            });
            if (audible == items.end()) return;
            std::lock_guard<std::mutex> sourceLock(sourceMutex);
            const auto identity = rowOwner.sourceToken(audible->owner ? audible->owner : audible->stream);
            if (!identity || *identity != selection.second.token) return;
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
            const auto token = rowOwner.sourceToken(owner);
            if (!token) return; // No speculative identity matching.
            if (before.empty() || before.back().token != *token)
                before.push_back({*token, {}});
            before.back().uris.push_back(entry.uri);
            entries[*token].push_back(entry);
            if (entry.stream == current) anchor = *token;
            if (const auto slot = window.find(*token)) rowOwner.locate(owner, slot->index);
        }
    }
    // An app burst may select another row while the preceding fresh producer
    // is being paired. Fence edits around the row actually selected now.
    const auto navigationRow = entries.find(navigationToken);
    const bool stableNavigationRow = lmsNavigation && navigationItem && !navigationItem->originMs &&
        navigationRow != entries.end() && navigationRow->second.size() == 1 &&
        !navigationRow->second.front().originMs &&
        navigationRow->second.front().sourceUrl == navigationItem->sourceUrl;
    if (lmsNavigation && !stableNavigationRow) {
        const auto stamp = std::chrono::steady_clock::now();
        const auto position = actions.position().response;
        const auto evidence = speakerChanges->poll(stamp,
            {{"CurrentTrack", position.value("Track")}, {"CurrentTrackURI", position.value("TrackURI")},
             {"RelativeTimePosition", position.value("RelTime")}});
        std::lock_guard<std::mutex> sourceLock(sourceMutex);
        for (const auto &entry : items) if (entry.uri == evidence.at("CurrentTrackURI")) {
            const auto token = rowOwner.sourceToken(entry.owner ? entry.owner : entry.stream);
            if (token) anchor = *token;
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
    const auto selected = rowOwner.selectionAt(audible->index);
    const bool rewindEcho = lmsNavigation && selected &&
        selected->token == navigationToken && selected->rewindMs.has_value();
    const bool speakerSelection = selected &&
        selected->token == navigationToken && selected->phase == Selection::Producing &&
        !selected->rewindMs;
    if (lmsNavigation && navigationFrom == audible->index && !speakerSelection) {
        const auto records = queue.snapshot();
        const auto producer = std::find_if(records.begin(), records.end(), [&navigationItem](const auto &r) {
            return r.source.occurrence == navigationItem->stream;
        });
        auto &target = entries[navigationToken];
        if (producer != records.end() && producer->seekPositionMs) {
            const auto selection = rowOwner.selectionAt(audible->index);
            const bool rewind = rewindEcho;
            // The admission status can precede LMS installing the new HTTP
            // stream's seek clock. Use the post-decode status while LMS still
            // waits for playback; it names this same audible occurrence.
            const auto seekMs = rewind ? uint32_t(*selection->rewindMs) :
                snapshot.waitingToPlay && snapshot.index == audible->index ?
                    snapshot.timeMs : producer->seekPositionMs;
            const auto rendererMs = target.size() == 1 ?
                rowOwner.seekWithin(target.front(), *producer, seekMs) : std::nullopt;
            if (rendererMs) {
                auto retained = std::find_if(items.begin(), items.end(), [&](const auto &e) {
                    return e.uri == target.front().uri;
                });
                if (retained == items.end()) throw std::logic_error("seek target missing");
                logQueue("window seek lms_index=" + std::to_string(audible->index) +
                    " position_ms=" + std::to_string(seekMs) +
                    " decision=in-item landed_frame=" + std::to_string(*producer->landedFrame) +
                    " warmup=" + std::to_string(producer->warmupFrames) +
                    " wav_origin=" + std::to_string(producer->wavOrigin) +
                    " attach_offset=" + std::to_string(producer->attachOffset) +
                    " produced_frames=" + std::to_string(producer->item->producedFrames()) + " lms_commands=0");
                rowOwner.rebindClock(size_t(retained - items.begin()), navigationItem->stream);
                current = retained->stream;
                if (navigationItem->uri != retained->uri) owned.erase(navigationItem->uri);
                queue.reanchorSpeaker(current, retained->originMs, seekMs);
                const bool superseded = rowOwner.queuedSelectionForOtherRow(navigationToken);
                if (!superseded && !rewind) {
                    actions.seekTime(*rendererMs);
                    rendererSeekPosition = RendererSeekPosition{*rendererMs, positionMonotonic()};
                }
                lastElapsed = *rendererMs / 1000;
                speakerChanges->selected();
                fastPoll();
                unsigned history = 0;
                for (const auto &slot : window.around(audible->index))
                    if (slot.index < audible->index && entries.count(slot.token)) ++history;
                logQueue("window lms-nav from_index=" + std::to_string(navigationFrom) +
                    " to_index=" + std::to_string(audible->index) +
                    " action=slide history=" + std::to_string(history) + " lms_commands=0 kind=seek edits=0");
                if (rewindEcho) requestStatus();
                if (rewindEcho) logQueue("window pairing lms_index=" + std::to_string(audible->index) +
                    " decision=paired reason=speaker-rewind elapsed_ms=" + std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - selected->issued).count()) +
                    " lms_commands=0");
                rowOwner.retireSelection(audible->index);
                rowOwner.retireAppEcho(navigationToken);
                rowOwner.clearReplacement(navigationToken);
                lmsNavigation = navigationSelecting = false;
                rowOwner.clearNavigationTarget();
                publishedWindowIndex = audible->index;
                playlist = std::move(snapshot);
                publishedWindowPolicy = window.policy();
                windowDirty = false;
                publish();
                return;
            }
            logQueue("window seek lms_index=" + std::to_string(audible->index) +
                " position_ms=" + std::to_string(seekMs) +
                " decision=fallback reason=" + producer->seekReason + " lms_commands=0");
        }
    }
    bool reused = false, bound = false, deferredUpcoming = false;
    bool selectedTarget = false;
    std::vector<Entry> retired;
    std::vector<std::shared_ptr<queue_audio::LazyItem>> retiredLazy;
    if (lmsNavigation) {
        auto &target = entries[navigationToken];
        // Pair the sink's verified same-source item at its existing URI.
        // A changed source coordinate or declaration gets a fresh URI.
        const auto lazy = rowOwner.lazyRow(navigationToken);
        bool published = false, declarationMismatch = false;
        if (target.size() == 1 && !target.front().item && lazy) {
            try {
                published = rowOwner.reproduceNavigation(target.front());
            } catch (const queue_audio::LazyItem::DeclarationMismatch &) {
                declarationMismatch = true;
            }
        }
        if (published) {
            // The controller supplied this producer. Keep a waiting PCM GET on
            // its existing route instead of cancelling and selecting it again.
            if (navigationItem->uri != target.front().uri) owned.erase(navigationItem->uri);
            rowOwner.retireSelection(audible->index);
            bound = true;
            if (const auto reason = rowOwner.replacementReason(navigationToken); reason && *reason != "evicted") {
                logQueue("window target re-produced lms_index=" + std::to_string(audible->index) +
                    " origin_ms=" + std::to_string(navigationItem->originMs) +
                    " lms_commands=" + std::to_string(rowOwner.appCommands(navigationToken)) + " reason=" + *reason);
            }
        } else if (target.size() == 1 && target.front().item &&
            target.front().item == navigationItem->item && !target.front().originMs &&
            !navigationItem->originMs && target.front().sourceUrl == navigationItem->sourceUrl) {
            rowOwner.rebindClock(target.front(), navigationItem->stream);
            if (navigationItem->uri != target.front().uri) owned.erase(navigationItem->uri);
            reused = true;
        } else {
            const auto reason = target.empty() ? "incomplete" :
                target.front().item && target.front().item->evicted() ? "evicted" :
                target.front().originMs || navigationItem->originMs ? "seek-partial" :
                target.front().sourceUrl != navigationItem->sourceUrl ? "source-mismatch" : "incomplete";
            auto replacement = *navigationItem;
            if (!declarationMismatch && target.size() == 1 && lazy &&
                target.front().originMs == navigationItem->originMs) {
                try {
                    if (!rowOwner.replaceNavigationWav(target.front(), replacement)) return;
                    replacement.uri = target.front().uri;
                    replacement.didl = target.front().didl;
                    if (navigationItem->uri != replacement.uri) owned.erase(navigationItem->uri);
                    bound = true;
                } catch (const queue_audio::LazyItem::DeclarationMismatch &) {
                    declarationMismatch = true;
                }
            }
            if (!bound) {
                if (declarationMismatch)
                    logQueue("window pairing lms_index=" + std::to_string(audible->index) +
                        " decision=fallback reason=immutable-wav-mismatch");
                retired.insert(retired.end(), target.begin(), target.end());
                if (lazy) retiredLazy.push_back(lazy);
                const auto row = makeRow(*audible, &replacement);
                owned.erase(replacement.uri);
                replacement.uri = row.uri;
                replacement.didl = row.didl;
            }
            target = {replacement};
            rowOwner.noteReplacement(navigationToken, declarationMismatch ? "immutable-wav-mismatch" : reason);
            if (std::string(reason) != "evicted")
                logQueue("window target re-produced lms_index=" + std::to_string(audible->index) +
                    " origin_ms=" + std::to_string(navigationItem->originMs) +
                    " lms_commands=" + std::to_string(rowOwner.appCommands(navigationToken)) + " reason=" + *rowOwner.replacementReason(navigationToken));
        }
    }
    const auto superseded = lmsNavigation && rowOwner.queuedSelectionForOtherRow(navigationToken);
    if (lmsNavigation) {
        const auto outgoing = std::find_if(items.begin(), items.end(), [this](const auto &entry) {
            return entry.stream == current;
        });
        const auto &target = entries.at(navigationToken).front();
        const auto ready = std::find_if(items.begin(), items.end(), [&](const auto &entry) {
            return entry.uri == target.uri;
        });
        if (!superseded && !rewindEcho && outgoing != items.end() && outgoing->originMs && ready != items.end() &&
            outgoing->uri != target.uri) {
            // Move once to the declared target before retiring the outgoing
            // seek suffix. History must already be safe when this move commits.
            actions.seekTrack(unsigned(ready - items.begin()) + 1);
            selectedTarget = true;
            current = target.stream;
            anchor = navigationToken;
        }
    }
    for (const auto &slot : window.around(audible->index)) {
        auto found = entries.find(slot.token);
        bool unavailable = false, evicted = false;
        if (found != entries.end() && !found->second.empty() && found->second.front().item) {
            const auto &entry = found->second.front();
            evicted = entry.item->evicted();
            try {
                unavailable = evicted || (entry.item->finished() &&
                    entry.item->descriptor().kind == queue_audio::PromiseKind::Exact &&
                    entry.item->producedFrames() < entry.frames);
            } catch (const queue_audio::ItemCancelled &) {
                unavailable = true;
            }
        }
        if (slot.index != audible->index && found != entries.end() &&
            !found->second.empty() && found->second.front().item &&
            (found->second.front().originMs || unavailable)) {
            // A seek/interrupted suffix is valid history, but cannot supply a later
            // full play of that parent from the beginning.
            if (slot.token == anchor || std::any_of(found->second.begin(), found->second.end(),
                            [this](const auto &entry) { return entry.stream == current; })) {
                // Back makes the still-audible parent upcoming. Select the
                // target before retiring that parent's interrupted suffix.
                deferredUpcoming = true;
            } else {
                rowOwner.noteReplacement(slot.token, found->second.front().originMs ? "seek-partial" : evicted ? "evicted" : "incomplete");
                if (found->second.front().originMs) {
                    if (const auto lazy = rowOwner.lazyRow(slot.token)) retiredLazy.push_back(lazy);
                    retired.insert(retired.end(), found->second.begin(), found->second.end());
                    entries.erase(found);
                    found = entries.end();
                } else if (found->second.size() == 1 &&
                           rowOwner.invalidateAudio(slot.token, found->second.front()) && evicted) {
                    logQueue("cache evicted lms_index=" + std::to_string(slot.index));
                }

            }
        }
        if (found == entries.end()) {
            entries[slot.token].push_back(makeRow(slot));
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
        coordinator.read([&] {
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
        });
    };
    io.insert = [this, &entries](auto token, auto segment, auto ordinal) {
        const auto &entry = entries.at(token).at(segment);
        const auto reply = actions.add(entry.uri, entry.didl, ordinal);
        if (reply.response.value("NumTracksAdded") != "1")
            throw std::runtime_error("queue-window-enqueue-unconfirmed");
    };
    io.insertMany = [this, &entries](const auto &tokens, auto update, auto ordinal) {
        std::vector<std::pair<std::string, std::string>> rows;
        for (auto token : tokens) {
            const auto &entry = entries.at(token).front(); rows.emplace_back(entry.uri, entry.didl);
        }
        if (!batchUnavailable) {
            try {
                const auto reply = actions.addMultiple(rows, update, ordinal);
                if (reply.response.value("NumTracksAdded") != std::to_string(rows.size()))
                    throw std::runtime_error("queue batch enqueue unconfirmed");
                logQueue("mirror batch accepted count=" + std::to_string(rows.size()));
                return;
            } catch (const std::exception &e) {
                // A definitive SOAP fault permits fallback. An uncertain HTTP
                // reply must never duplicate rows.
                const std::string error = e.what();
                if (error.find("fault=") == std::string::npos) throw;
                batchUnavailable = true;
                logQueue("mirror batch rejected; AddURIToQueue fallback");
            }
        }
        for (const auto &row : rows) {
            const auto reply = actions.add(row.first, row.second, ordinal++);
            if (reply.response.value("NumTracksAdded") != "1") throw std::runtime_error("queue enqueue unconfirmed");
        }
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
    std::vector<unsigned> partialReplacements;
    unsigned replacedOrdinal = 1;
    for (const auto &old : before) {
        const auto next = std::find_if(wanted.begin(), wanted.end(), [&](const auto &p) { return p.token == old.token; });
        if (next != wanted.end() && old.uris != next->uris && rowOwner.replacingShortenedRow(old.token))
            partialReplacements.push_back(replacedOrdinal);
        replacedOrdinal += old.uris.size();
    }
    std::optional<uint64_t> editAnchor = anchor;
    bool overlap = false;
    try {
        if (lmsNavigation && anchor == navigationToken && !superseded) {
            auto old = std::find_if(before.begin(), before.end(), [navigationToken](const auto &parent) {
                return parent.token == navigationToken;
            });
            const auto target = std::find_if(wanted.begin(), wanted.end(), [navigationToken](const auto &parent) {
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
                if (!rewindEcho) {
                    // The speaker's queue already includes the target, while
                    // items still describes the outgoing row at this ordinal.
                    speakerChanges->expectTrack(audible->index, navigationToken, target->uris.front(),
                        ordinal, unsigned(expected.size()), "x-rincon-queue:" + room.uuid + "#0");
                    actions.seekTrack(ordinal);
                }
                selectedTarget = true;
                const auto latest = check();
                io.remove(latest.update, ordinal + target->uris.size(), old->uris.size());
                expected.erase(expected.begin() + ordinal - 1 + target->uris.size(),
                               expected.begin() + ordinal - 1 + target->uris.size() + old->uris.size());
                if (check().update == latest.update) throw std::runtime_error("queue window removal unconfirmed");
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
        const bool unchanged = before.size() == wanted.size() && std::equal(before.begin(), before.end(), wanted.begin(),
            [](const auto &a, const auto &b) { return a.token == b.token && a.uris == b.uris; });
        if (!unchanged) Edits(std::move(io)).apply(before, wanted, editAnchor);
        for (auto ordinal : partialReplacements)
            logQueue("window replace ordinal=" + std::to_string(ordinal) + " reason=seek-partial edits=2");
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
        rowOwner.commitLayout(std::move(rebased));
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
            token = rowOwner.sourceToken(owner).value();
        }
        releaseWindowParent(token, rowOwner.navigationFirst());
        route.retire(entry.uri);
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
        if (const auto lazy = rowOwner.lazyRow(parent.token)) lazy->cancel();
        queue.releaseParent(entry.owner ? entry.owner : entry.stream);
        releaseWindowParent(parent.token);
        // A cached target may have a newer LMS producer clock while serving
        // its retained older audio. Retire both when the parent leaves.
        if (entry.owner && entry.owner != entry.stream) queue.releaseParent(entry.stream);
        for (const auto &uri : parent.uris) { route.retire(uri); owned.erase(uri); }
    }
    rowOwner.commitLayout(std::move(reordered));
    if (lmsNavigation) {
        const auto target = std::find_if(items.begin(), items.end(), [&](const auto &entry) {
            return entry.stream == navigationItem->stream;
        });
        if (target == items.end()) throw std::logic_error("navigation target missing");
        if (!superseded) {
            current = target->stream;
            const auto selection = rowOwner.selectionAt(audible->index);
            // The index/token-keyed Selection owns a rewind echo. Its elapsed
            // position can already have advanced and is not new seek authority.
            const bool rewindAtTarget = rewindEcho;
            if (!selectedTarget && !rewindEcho && !rewindAtTarget) actions.seekTrack(unsigned(target - items.begin()) + 1);
            lastElapsed = rewindAtTarget ? *selection->rewindMs / 1000 : 0;
            expectedState = "PLAYING";
            if (!rewindEcho) {
                if (state != "PLAYING") actions.play();
                waitingForPlay = true;
                playAt = std::chrono::steady_clock::now();
            }
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
            std::to_string(rowOwner.appCommands(navigationToken)) + " kind=" +
            (navigationFrom == audible->index ? (snapshot.timeMs < 500 ? "previous" : "seek") : navigationFrom + 1 == audible->index ? "next" :
             navigationFrom == audible->index + 1 ? "previous" : "jump") +
            " edits=" + std::to_string(queueEdits - navigationEdits));
        if (const auto reason = rowOwner.replacementReason(navigationToken); reason)
            logQueue("window pairing lms_index=" + std::to_string(audible->index) +
                " decision=re-produced reason=" + *reason +
                " lms_commands=" + std::to_string(rowOwner.appCommands(navigationToken)));
        else if (reused || bound) logQueue("window pairing lms_index=" + std::to_string(audible->index) +
            " decision=paired lms_commands=" + std::to_string(rowOwner.appCommands(navigationToken)));
        if (reused) logQueue("window retained target audio reused lms_commands=0");
        if (bound) logQueue("window lazy target producer adopted lms_commands=0");
        if (rewindEcho) requestStatus();
        if (rewindEcho) logQueue("window pairing lms_index=" + std::to_string(audible->index) +
            " decision=paired reason=speaker-rewind elapsed_ms=" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - selected->issued).count()) +
            " lms_commands=0");
        rowOwner.clearReplacement(navigationToken);
        rowOwner.retireSelection(audible->index);
        lmsNavigation = navigationSelecting = false;
        rowOwner.clearNavigationTarget();
    } else if (publishedWindowIndex && *publishedWindowIndex != audible->index) {
        unsigned history = 0;
        for (const auto &parent : wanted) {
            if (parent.token == audible->token) break;
            ++history;
        }
        logQueue("window lms-nav from_index=" + std::to_string(*publishedWindowIndex) +
            " to_index=" + std::to_string(audible->index) + " action=slide history=" +
            std::to_string(history) + " lms_commands=" + std::to_string(rowOwner.appCommands(audible->token)) + " kind=" +
            (rowOwner.appCommands(audible->token) ?
                (*publishedWindowIndex + 1 == audible->index ? "next" :
                 *publishedWindowIndex == audible->index + 1 ? "previous" : "jump") : "natural"));
    }
    rowOwner.retireAppEcho(audible->token);
    publishedWindowIndex = audible->index;
    playlist = std::move(snapshot);
    publishedWindowPolicy = window.policy();
    refreshUpcoming = deferredUpcoming;
    windowDirty = deferredUpcoming;
    if (deferredUpcoming) nextWindowPoll = {};
    if (generationRebuildEdits && !deferredUpcoming) {
        logQueue("window rebuild reason=playlist-change edits=" + std::to_string(queueEdits - *generationRebuildEdits));
        generationRebuildEdits.reset();
    }
    publish();
}
bool QueueRoom::Impl::acceptSnapshot(queue_audio::PlaylistSnapshot &snapshot) {
    const auto known = playlist.entries.size();
    const auto busy = [this] {
        std::lock_guard<std::mutex> lock(mutex);
        return selecting || lmsNavigation || navigationSelecting || rowOwner.hasSelections() ||
            selectionDeferred.load() || !commands.empty() || windowDirty ||
            navigationDeadline != std::chrono::steady_clock::time_point{} ||
            std::chrono::steady_clock::now() < ownUntil;
    };
    using Decision = queue_audio::QueueRowOwner::SnapshotDecision;
    const auto decision = rowOwner.snapshotDecision(playlist, snapshot, snapshot.entries.size() < known && busy());
    if (decision == Decision::ConfirmReplacement) {
        auto fresh = reader.read(host, mac);
        if (rowOwner.confirmsReplacement(snapshot, fresh)) {
            snapshot = std::move(fresh);
            logQueue("window snapshot accepted reason=new-playlist-generation");
            return true;
        }
    }
    if (decision == Decision::RejectShort || decision == Decision::ConfirmReplacement) {
        logQueue("window snapshot rejected reason=short-in-flight known=" + std::to_string(known) +
                 " received=" + std::to_string(snapshot.entries.size()));
        return false;
    }
    if (decision == Decision::ConfirmClear) {
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
    const auto existing = rowOwner.selectionAt(slot->index);
    if (existing && existing->token == token) {
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
        const auto lazy = rowOwner.lazyRow(token);
        if (lazy && !lazy->ready()) {
            auto first = queue.latestOccurrence() + 1;
            std::lock_guard<std::mutex> sourceLock(sourceMutex);
            for (const auto &record : queue.snapshot()) {
                const auto identity = rowOwner.sourceToken(record.source.occurrence);
                if (!record.parent && record.incomplete.empty() && record.fault.empty() &&
                    identity && *identity == token)
                    first = std::min(first, record.source.occurrence);
            }
            rowOwner.bindSelection(slot->index, token, first);
        }
        logQueue("selection bind lms_index=" + std::to_string(slot->index) + " reason=already-selected lms_commands=0");
        return;
    }
    rowOwner.enqueueSelection(slot->index, token);
    logQueue("selection queued lms_index=" + std::to_string(slot->index) + " reason=" + reason);
    advanceSelections();
}
void QueueRoom::Impl::selectionPublished(unsigned index) {
    if (rowOwner.acknowledgePublication(index)) fastPoll();
}
void QueueRoom::Impl::advanceSelections() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!commands.empty() || selecting || selectionDeferred.load()) return;
    }
    // LMS has one producer. Drain each live declaration before selecting the
    // next index, so a rapid burst cannot cancel an earlier HTTP promise.
    for (const auto &[index, selection] : rowOwner.selectionSnapshot()) {
        const auto lazy = rowOwner.lazyRow(selection.token);
        if (selection.phase != Selection::Published && lazy && lazy->abandoned()) {
            logQueue("selection retired reason=reader-left lms_index=" + std::to_string(index));
            rowOwner.retireSelection(index);
        }
    }
    if (rowOwner.hasProducingSelection()) return;
    while (const auto next = rowOwner.dequeueSelection()) {
        const auto index = *next;
        const auto selection = rowOwner.selectionAt(index);
        if (!selection) continue;
        // A natural start for this same occurrence may arrive before LMS's
        // playlist index advances. Parse its declaration before our one app
        // selection cancels it; never restart a half-admitted codec header.
        const auto latest = queue.latestOccurrence();
        const auto records = queue.snapshot();
        if (!rowOwner.drainDeclarations(records)) {
            rowOwner.deferSelection(index);
            return;
        }
        const auto producer = std::find_if(records.begin(), records.end(), [&](const auto &record) {
            return record.source.occurrence == latest && record.sourceToken == selection->token &&
                   record.incomplete.empty() && record.fault.empty();
        });
        if (producer != records.end() && (!producer->item ||
            (producer->pairedFrom && producer->actual - producer->attachOffset < producer->verificationFrames))) {
            rowOwner.deferSelection(index);
            return;
        }
        Selection issued;
        {
            std::lock_guard<std::mutex> lock(mutex);
            issued = rowOwner.issueSelection(index, queue.latestOccurrence() + 1, std::chrono::steady_clock::now());
        }
        rowOwner.noteAppSelection(issued.token, issued.first);
        if (issued.rewindMs) {
            roomLmsCommand(host, mac, "time " + std::to_string(*issued.rewindMs / 1000),
                           "speaker-rewind", logQueue);
            logQueue("selection issued lms_index=" + std::to_string(index) + " reason=speaker-rewind lms_commands=1");
            return;
        }
        const auto lazy = rowOwner.lazyRow(issued.token);
        roomLmsCommand(host, mac, "playlist index " + std::to_string(index),
                       lazy && lazy->waiting() ?
                           "lazy-pcm-get" : "speaker-nav", logQueue);
        if (lazy) {
            lazy->commandIssued();
            logQueue("lazy activated lms_index=" + std::to_string(index) + " lms_commands=1");
        }
        rowOwner.noteAppCommand(issued.token);
        logQueue("selection issued lms_index=" + std::to_string(index) + " lms_commands=1");
        return;
    }
}

void QueueRoom::Impl::activateLazy(uint64_t token) {
    append();
    const auto found = rowOwner.lazyRow(token);
    if (!found || !window.find(token) || !found->claimActivation()) return;
    arbitrateSelection(token, "lazy-pcm-get");
}

QueueRoom::Impl::Entry QueueRoom::Impl::makeRow(const queue_audio::QueueWindow::Slot &slot,
                                             const Entry *produced, unsigned bits, unsigned knownRate) {
    const auto lazy = rowOwner.prepareLazyRow([this, token = slot.token] {
        std::lock_guard<std::mutex> lock(mutex);
        if (closing) return;
        commands.push_back({'L', token});
        this->changed.notify_all();
    });
    const auto kind = produced ? produced->item->descriptor().kind :
        remotePlaybackSource(slot.track.id, slot.track.remote, slot.track.url)
            ? queue_audio::PromiseKind::UpperBound : queue_audio::PromiseKind::Exact;
    upnp::QueueMetadata metadata;
    const auto rate = produced ? produced->item->descriptor().rate : (knownRate ? knownRate : slot.track.sampleRate);
    const auto format = route.formatFor(kind, rate);
    metadata.bits = format == queue_audio::Format::Flac ?
        (produced ? produced->item->descriptor().bits : bits) : 24;
    metadata.url = speaker->controllerUri() + route.publishDeferred(1, "lazy-" + std::to_string(slot.token) + "-" + std::to_string(++lazySerial),
        [lazy, token = slot.token](upnp::StreamRequest &request) {
            const auto start = std::chrono::steady_clock::now();
            auto item = lazy->resolve(request, std::chrono::seconds(3));
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
            std::printf("yeney: queue lazy %s token=%llu elapsed_ms=%lld lms_commands=%u\n",
                item ? "headers" : (ms >= 2990 ? "deadline" : "abandoned"),
                (unsigned long long)token, (long long)ms, lazy->commandCount());
            return item;
        }, [lazy](bool audio) { return audio ? lazy->readyAudio() : lazy->ready(); }, kind, metadata.bits, rate);
    metadata.session = session + "-" + std::to_string(slot.token);
    metadata.title = slot.track.title; metadata.artist = slot.track.artist; metadata.album = slot.track.album;
    metadata.artwork = slot.track.artwork(host);
    metadata.rate = 1; // Duration scale only; deferred DIDL omits sampleFrequency.
    metadata.frames = uint64_t(slot.track.duration);
    metadata.deferred = true;
    if (produced) {
        const auto &descriptor = produced->item->descriptor();
        metadata.frames = (produced->item->contentLength().value() - 44) / 6;
        metadata.rate = descriptor.rate;
        metadata.size = queue_audio::rowLength(format, metadata.frames, metadata.rate, metadata.bits);
        metadata.deferred = false;
        metadata.preciseDuration = produced->item->descriptor().kind == queue_audio::PromiseKind::UpperBound;
        if (!lazy->publish(produced->item, produced->stream)) throw std::logic_error("partial row declaration rejected");
    }
    upnp::sanitizeXmlFields({{"title", &metadata.title}, {"artist", &metadata.artist},
        {"album", &metadata.album}, {"artwork", &metadata.artwork}});
    // Placeholder identities descend from the top; core continuations grow
    // upward from the midpoint. Never alias their first continuation IDs.
    const auto id = UINT64_MAX - slot.token;
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        rowOwner.registerPlaceholder(id, slot.index, slot.token);
    }
    rowOwner.registerLazyRow(slot.token, lazy);
    Entry result{id, metadata.url, {}, slot.track.url, 0, metadata.title, 0, 0, upnp::queueDidl(metadata)};
    owned.insert(metadata.url);
    return result;
}

} // namespace bridge
