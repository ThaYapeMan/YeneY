# Sonos queue mode

Status: phases 1 and 2 implemented independently, 16 October 2026 field corrections. Queue playback is
not connected yet; the existing stream transport remains in use. The owner selected variant A: LMS remains the player and audio processor;
Sonos plays one HTTP queue item per LMS track. Stream mode remains available.
The required playback policy is `YENEY_PLAYBACK_MODE=auto|stream|queue`, default
`auto`. Crossfade must work in queue mode, primarily through yeney-core mixing.
The standalone item store/WAV handler and processing-aware packing now exist.
Queue control, playlist mirroring and sink integration below remain implementation plans.

## Evidence and acceptance limits

Study is a Play:1 with S2 firmware 86.10. The three owner-run probes are 17:19,
17:46 and 18:05 on 2026-10-12. Q1–Q6 returned GO in all three; 18:05 also
returned GO for grouped Q7. The 17:46 run stopped after Q6 because MBR's YeneY
service was active: it is not evidence of a failed audio capability.
[Trimmed evidence and provenance](../../tests/fixtures/queue-probe-boundaries-2026-10-12/README.md)
retain source line numbers and hashes of the untouched original files.

Corrected next-item request lead, seconds, first / second join:

| Run | Q1 same rate | Q2 mixed rate | Q6 position trial |
|---|---:|---:|---:|
| 17:19 | 3.198 / 4.028 | 3.804 / 3.353 | 3.269 / 3.631 |
| 17:46 | 3.681 / 3.559 | 4.035 / 3.648 | 3.105 / 3.254 |
| 18:05 | 3.122 / 3.369 | 3.605 / 3.647 | 3.800 / 3.251 |

18:05 Q7: same rate 3.356 / 3.256; mixed rate 3.115 / 3.517. These are
midpoints, not acoustic measurements. All 22 midpoint estimates are
3.105–4.035 s; their uncertainty intervals span 2.354–4.723 s. With last old-item
sample `(send, receive, rel)` and duration D, estimated end lies in
`[send + D - rel - 1, receive + D - rel]`; subtract the first successor GET.
RelTime is floored to seconds. The old ~93 s first-join estimates selected the
terminal rewind to item 1, RelTime 0, with a temporarily cached PLAYING state.
The checker now uses only the first visit, before any different current URI.

The latest owner summary says all standalone same- and mixed-rate joins were
clean. Retain the earlier 17:19 Q6 note mentioning a small frequency-change
click too; repeat native-rate standalone acceptance rather than discarding that
note. Grouped same-rate joins were clean; grouped mixed-rate joins had “just a
tiny click when change frequencies”. Automatic GO does not overrule that note:
grouped native-rate switching is **GO WITH CONDITIONS**.

Other observations change the implementation requirements:

* FLAC and WAV, 16 and 24 bit, play with finite lengths. Some 30 s responses
  completed at 8–9x real time, beyond the common approximately 5x FLAC fetch.
  Socket write completion is not a TCP acknowledgement or acoustic measurement.
* Exactly 1.0x paced FLAC worked; reported maximum socket-write ahead was
  0.779, 0.953 and 0.881 s. This proves tolerance of a paced response, not
  next-track production readiness. It does not yet qualify paced WAV24.
* After a WAV seek to 12 s, requested offsets were 10,518,528, 11,010,048 and
  9,322,496 bytes. With the tested 44-byte WAV16 stereo header they map to
  59.629, 62.415 and 52.848 s. Fresh RelTime was 12. Byte alignment and file-time
  mapping are exact; **a Range request is not the requested audible seek time**.
* WAV resumed without another request because the response had already finished
  before Pause. FLAC closed around Pause and made new GET/Range requests after
  Resume. Support cached playback, persistent sockets and reconnection equally.
* RelTime restarts for each item; 61, 59 and 71 AVTransport notifications arrived.
  Events and 4 Hz position polling provide complementary identity and clock data.

### LMS read-ahead field results, 16 October

The 17:44 ALAC run (owner-stopped early) and 17:53 FLAC album run exposed an LMS
song-model ordering requirement. LMS sent a successor strm 17–19 ms after STMd,
without waiting for STMs, and retained at most two tracks beyond the audible
one. The measured completed successor leads were approximately 198, 446 and
509 seconds. The old `INSUFFICIENT; worst lead -1 s` included unobserved ends
and an initial failed source; it did not demonstrate inadequate read-ahead.
The supplied evidence folder has logs and TSV but no `dryrun-off.md`.

**Queue ordering rule: STMs(N) before STMd(N).** Decoding may run ahead, but
completion stays attached to N until its explicit audible start. Send a held
STMd immediately after STMs. LMS associates STMs with the currently streamed
song: sending STMd first let LMS stream N+1 before STMs(N), making its player
model run ahead of the audio. Both field runs showed this order and the owner
observed apparent skipping near track one. The fake SlimProto server now
models that association and the two-ahead ceiling. Expected successor lead is
roughly the predecessor's remaining audible duration; qualify that on LMS.

The old startup wait also sealed every album item: it awaited N+1's strm, which
only arrived after 18–25 seconds of decoding N. Commit the outgoing transition
from N's own strm settings as soon as its exact count is known. Type zero
immediately promises no overlap. Crossfade predicts the current period with
short-track clamping; later contradictory type/period, rate or smart eligibility
uses the retained-tail policy below and never changes a promised length.
Unknown counts stay sealed. If an unknown-count item cannot start before
sealing, its available pre-start audible time is zero: resolve the full tail as
`sealed-startup`, explicitly degrading that overlap rather than deadlocking the
STMs/STMd/successor exchange. Target local exact-count FLAC/ALAC startup and STMs
within two seconds. Hold a configured overlap tail for a late successor until
the remaining audible deadline minus 100 ms, then log `successor-deadline`.
With transition zero the normal final reason is `no-transition`.

The initial 17:53 strm arrived immediately after HELO, before owner action;
its origin remains unconfirmed. It was MP4/ALAC, exact count 2,428,264, with
2,420,736 frames recorded before a truncated-top-level-box error. The same FLAC
track later played without faults. The logs contain no MP4 payload, so they
cannot prove the malformed header bytes. Synthetic regular/trailing-box and
LMS-style rewritten-seek/partial-mdat fixtures reproduce the EOF failure class.
Queue EOF accepts complete selected samples despite a retained larger mdat
length. Drain decoder output before evaluating an error; checking it first
could discard the final 8,192-frame decoder buffer. An exact short source pads
at most one second with logged `source-short <n> frames`; larger deficits use
the item length-fault policy. STMn waits until that occurrence's valid PCM has
finished playing. The late STMs after occurrence 2's strm was attributed by the
TSV to occurrence 1, whose buffered audio had survived its premature STMn.
Queue flush now fences pending starts; zero-frame occurrences cannot send STMs
or manufacture a silence-only padded item. Resolve an unplayed failed source
and retire its pending descriptor even when a successor is already queued.
After STMn, suppress the failed source's STMu so it cannot stop the new LMS
request.

STMr has a single emitter, the shared strm-u acknowledgement; DSCO does not
emit it in either capability. The old logs did not record incoming u commands.
Queue command logging and the no-unpause regression now distinguish those
acknowledgements from an unsolicited readiness message.

The dry-run excludes incomplete pairs from its lead verdict, preserves their
run-end/source-fault/flushed reasons and reports codec and startup delay. A
separate 1 Hz raw-MAC LMS CLI sampler records playlist index/time/title without
blocking the simulated audible clock. During retained predecessor prefixes,
CLI identity remains the outgoing source until incoming source frames actually
start. A pair remains incomplete until its retained prefix/continuation ends;
the reported audible end includes all of that source's PCM. Sustained index disagreement over two
seconds fails as `lms-track-mismatch`. Owner runs start at index zero after
Stop/clear, without shuffle/repeat or playlist edits. See
[complete four-run commands](../../DEVICE-VERIFICATION.md#queue-mode-phase-2-lms-read-ahead).
No raw owner evidence is committed and no deployment was performed.

## 1. Architecture and item representation

Keep LMS → yeney-core decode/ReplayGain/transitions → Sonos sink. Add a queue
sink and immutable per-item store alongside the existing stream sink. Do not
fetch original music directly from LMS's library endpoint: that would bypass the
player's processing and settings. Obtain playlist identity and metadata through
LMS CLI, matched to core track descriptors, not merely whichever title LMS
currently displays while decoding ahead.

Each playlist **occurrence**, including repeat visits to the same track ID, gets
an opaque item token. Proposed URL:
`http://<bridge>:<port>/music/queue.wav?session=<nonce>&epoch=<g>&item=<token>&revision=<v>`.
Register a fixed HTTP route, validate the query and room/session ownership, and
keep bytes immutable for that URL. Queue revisions, seeks that change rendered
content, ReplayGain changes and new processing settings require new assets.
A new HTTP request, including prefetch, must never create a new playback epoch.

DIDL uses `object.item.audioItem.musicTrack`, escaped title, artist/creator,
album, LMS cover URL `http://<lms>:9000/music/<id>/cover.jpg`, duration, size,
sampleFrequency, bitsPerSample and nrAudioChannels. Use native
`http-get:*:audio/wav:*`, not radio protocolInfo. Set the coordinator transport
once to `x-rincon-queue:<coordinator-udn>#0`; enqueue item URLs with metadata.
Sonos owns the joins; never SetAVTransportURI at every track boundary.

Recommend stereo PCM WAV24, native 44.1/48 kHz when standalone. With the proposed
canonical 44-byte PCM header, frame width is 6 bytes and exact entity length is
`44 + 6 * output_frames`. Offset B maps to file time `(B - 44)/(6 * output_rate)`
for B ≥ 44; header reads are not audio seeks. Accept byte ranges even when not
frame-aligned; translate only complete frames for diagnostics. Support HEAD,
200, 206, suffix/bounded ranges, 416 and exact Content-Range. Verify WAV24 seeks
on the device: Q5 tested WAV16, while Q3 qualified WAV24 playback only. Check
64-bit arithmetic and the classic RIFF 4 GiB limit; RF64 is unqualified.

Owner-selected storage is **A+D**, replacing the earlier sealed-WAV-on-disk
baseline. HTTP always exposes WAV24; produced history is stored losslessly as
FLAC, never WAV or raw PCM on disk. An indexed FLAC history is decoded back to
exact WAV byte coordinates. The 8 MiB per-room RAM write buffer holds the newest
packed PCM before spill. Independent 4096-frame FLAC chunks bound read-back
work and preserve byte identity across RAM/disk reads. Readers use a binary
frame index and one decoded-chunk cache each, separate from the write-buffer
budget; small reads do not repeatedly decode an entire item. libFLAC is already linked
and BSD-licensed; no additional codec dependency is introduced.

Unprocessed normalized 16/24-bit sources retain their source bits exactly in a
24-bit container. Processed 24-bit output rounds to nearest and clips, without
dither; processed 16-bit stream output uses seedable TPDF dither then rounding
and clipping. The core provides per-block processing provenance; the bridge must
not infer gain or transition state from sample values.

Current attachment points at baseline 5e8755b:

| Code | Current behavior and required separation |
|---|---|
| `third_party/yeney-core/core/player.cpp:119–190, 724–745` | Track descriptors, elapsed, audible events and exact `boundary(next)`; add stable occurrence and resolved-boundary/completion callbacks. |
| `core_sonos.cpp:46–86, 157–178, 212` | Stores boundary coordinates, stages PCM, replaces streams on rate change, sets startOnSubmit; queue sink must separate production from audibility. |
| `sbencoder.cpp:119–153, 286–295` | Fixed-rate FLAC header and approximately 250 ms encode lead constrain the current stream; queue assets bypass this pacing. |
| `sbstreamer.cpp:89–105, 340–412, 535–605` | One active encoder, retained stream ranges/restarts and chunked delivery; keep unchanged for stream mode. |
| `upnp/http_server.cpp:272–274`, `upnp/soap.cpp` | Fixed routes and radio DIDL; add a separate finite-item route and music-track DIDL. |

For a normal join, close item N immediately before the boundary callback's first
frame of N+1. For a crossfade, the callback is at overlap start, so the mixed
frames belong to N+1. Do not split by duration timers or by HTTP requests. The
old core track's `end` can include its overlap tail: queue item end and decoder
track end are distinct and need explicit records.

## 2. Delivery, production ahead and the reservoir

A: publish immediately when the source supplies an exact frame count: FLAC
STREAMINFO total_samples > 0, validated ALAC, or PCM/WAV/AIFF header counts.
The published promise includes a transition decision fixed at publication. HTTP
Content-Length is header plus promised output frames × 6; readers wait for bytes
not yet produced. Sources without an exact count (MP3, AAC, transcoded input,
FLAC total_samples = 0) are sealed before publication. Reliable count extraction
and transition planning are later core/sink integration work, not guessed by the
phase 1 store.

D: keep produced audio in per-item FLAC history, decoded to WAV24 on demand.
Many readers have independent byte positions; neither reads nor reconnects consume
producer data. Serve available bytes at the speaker's requested speed, including
8–9x bursts, then wait for production. Do not limit fetches to 5x or assume a
persistent socket. B, re-rendering through an LMS seek, is the fallback only
when history has been evicted. A seek target still comes from fresh position or
events, never from a Range offset alone.

Owner budgets: **8 MiB RAM write buffer per room**, **256 MiB disk quota per room**,
configurable through `YENEY_QUEUE_DISK_MIB`. The storage directory is
`/var/lib/yeney/queue/<room>` and is cleaned on store startup under an exclusive
room lock. When systemd supplies a single absolute `STATE_DIRECTORY`, the queue
root is `<STATE_DIRECTORY>/queue` instead; loopback acceptance uses this to keep
all storage under its temporary directory. Evict the oldest history first, never current or next. Phase 1 evicts
whole completed/failed item histories by creation order; live producers are
retained. If protected/live history alone exhausts quota, raise a storage fault
rather than lose its audio or exceed quota. Decoder/encoder scratch space is
bounded per chunk and separate from the RAM write-buffer budget. The encoded
stream buffer is still **256 packets** and is unchanged.

Phase 2 provides `CoreQueueSink`, constructed only by tests and
`yeney-queue-dryrun`. Its `decodeAhead()` capability enables a separate core path:
monotonic occurrence IDs, descriptor updates, decoder completion/STMd and resolved
item ends are separate records. `resolvedItemBoundary` uses the exact mixer
output coordinate; overlap PCM belongs to the incoming occurrence. Decoder-track
end remains a separate event after its retained source tail is consumed. STMs
requires an explicit `audibleFrom(occurrence, frame)` signal; HTTP reads, producer
writes and STMd never create one. STMt reads the sink's audible coordinate.
The existing stream capability remains unset and its fixture bytes are unchanged.
The standalone test/dry-run sink accepts rates up to 192 kHz; the future Sonos
controller must apply the actual coordinator format limits before preparation.

A configurable `futurePcmSeconds` pool defaults to 30 s, charged using each
track's own rate. In addition, two bounded mixer look-behind windows retain up to
`transitionMaxFrames` each (default 2,097,152 frames). This reservation is explicit:
30 s is the future pool, not a claim that total mixer RAM is 30 s. Without it,
a long fade can fill the future pool before decoder completion and deadlock the
STMd/successor exchange. The worker's PCM job queue separately defaults to 30 s
and includes in-flight jobs in admission; `write` returns partial acceptance or
zero without waiting for encoding/disk. Control jobs have an 8192-job hard cap.

All item-store mutation/FLAC encoding/disk writes/eviction runs on one room
worker. Store metadata locking is short: encode and history writes occur unlocked;
reader decoding occurs unlocked from an already-open inode, so eviction cannot
invalidate an in-progress file read. A blocked-write test proves producer
backpressure and reader independence. This supersedes phase 1's serialized
encoding under the room mutex. No production HTTP route or SOAP action is added.

Transition commitment predicts the outgoing mix from the current strm and
core's capped frame window, without waiting for a successor. Type zero promises
the complete source immediately. Publication transfers pending history to a new
immutable promised generation; it invalidates the unpublished handle and wakes
waiters. A later authoritative successor may disprove the prediction; retain
any excess tail and preserve the old promise. Unknown source counts stay
sealed. A short exact source may pad at most one second with an explicit
`source-short` fault; larger deficits use the length-fault policy. No silent
padding is normal transition recovery.

Qualify actual LMS successor delivery with the owner-run dry-run documented in
DEVICE-VERIFICATION.md. Never CLI-skip merely to move the producer ahead.

Preparation still needs approximately 12 s total audio lead and up to 8 s of
successor prefix ready around 5 s before the current end; these are engineering
targets to qualify. A 1x source needs initial headroom. The diagnostic first-item startup target is two seconds for local exact-count
FLAC/ALAC; the future production request deadline remains 15 seconds. Metadata-only queue entries may exist before preparation; on an actual
request, select the mapped LMS occurrence and prepare/serve within that deadline.
The standalone handler waits for future ranges, disconnects on faults/timeouts,
and does not register a production route. Unknown-count assets remain unpublished
until sealed. A late successor may introduce a gap; preserve the current tail,
log the reason and use controlled fallback rather than an early URI switch.

## 3. Exact length and mismatch policy

LMS decimal duration alone is not an exact count. A validated source count,
codec priming/padding removal, seek origin, output format and the transition
commitment determine promised output frames. Unknown-count sources are sealed;
then actual rendered count supplies the immutable WAV header and Content-Length.
An ALAC/container count must be validated, not assumed exact merely from a codec
name. The store accepts a promise from its caller; it does not parse source files.

Phase 2 decoder metadata supplies positive FLAC STREAMINFO counts; a retained-header
seek subtracts the first decoded frame's absolute sample coordinate. Headerless
FLAC seeks and zero total_samples remain unknown. ALAC uses validated sample tables
and the existing edit-list/iTunSMPB priming/padding trim. In the queue decoder, finite WAV/AIFF payloads stop at their validated frame count,
ignore trailing metadata and reject short payloads; the legacy stream decoder
retains its behavior. WAV data sizes must be frame-aligned and non-sentinel; AIFF COMM counts must agree with SSND bytes. Raw
PCM without a finite container count and MP3 remain unknown. AAC is not an
advertised native decoder. No decimal LMS duration is used.

Upstream transcoding cannot be identified from strm alone. The host can declare
`sourceIsTranscoded`, which suppresses exact counts even if a transcoder supplies
finite headers. The dry-run exposes `--transcoded` for this owner-controlled case;
the future playlist controller must supply authoritative source metadata. Likewise,
smart/same-album context is an optional host rule: without it the received strm
transition is authoritative, and type 0 is reported as type 0 rather than guessed
to mean smart crossfade. This is an explicit integration boundary, not a CLI
preference prediction in phase 2.

Publication commits the transition and item length. If a later playlist edit
cancels or shortens the anticipated fade, item N still ends at its promised
frame. The **next item carries the remaining previous-item tail** before the new
track, preserving continuous PCM without losing audio. Plan the next item's
length and piecewise source-time map to include that tail before committing its
header. Title handoff can precede incoming source time zero; LMS/LampaStream must
keep the incoming source clock at zero until incoming audio actually starts.
If an edit replaces the successor with a different-rate source, do not put the
old-rate tail under its WAV header or silently convert it. Use a tail-only
continuation as the next item at the old native rate, then the new track at its
native rate. This exceptional extra queue entry is linked to the old occurrence;
its clock advances the old source until the true incoming item starts. It avoids
losing audio while the group/conversion policy remains deferred. Qualify the
continuation insertion and any controlled requeue gap on the device.
Phase 2 implements this routing in the queue sink worker: same-rate residuals
become a successor prefix; different-rate residuals remain a tail-only asset.
The store still rejects an overrun. SOAP insertion/playlist revision handling is
phase 4 work; these assets are currently exercised only by tests and the dry-run.

Whole-playlist URL/DIDL placeholders do not authorize publishing speculative
HTTP lengths. If a successor has already had a conflicting HTTP promise or
cached body, preserve that immutable asset and move the residual into a new
revision/successor before handoff. A race that commits both incompatible promises
cannot guarantee every frame and both lengths: report the fault and take a
controlled fallback. No silent trimming or padding is permitted.

Phase 1 enforces each promise: append beyond its count or finish below it raises
`LengthFault` with expected/actual frames, wakes readers with the same failure,
and never serves extra frames. Short output is never silently padded. Cancellation
wakes all readers. Valid byte ranges, including unaligned or not-yet-produced
bytes, retain exact coordinates. Classic RIFF limits are checked before accepting
counts. Q9 remains diagnostic evidence of malformed-length consequences, not
permission to violate a promise.

## 4. Controls in both directions

A serialized room controller owns playlist revision, queue ownership, occurrence
mapping, core generation, audible item, pending commands and coordinator lease.
Callbacks enqueue observations; they do not perform blocking LMS/UPnP operations
under event or audio locks. Every asynchronous result is fenced by epoch and
revision. Use typed pending intents with expected target/state, deadline and
origin; acknowledge matching echoes without reissuing them. New contrary user
intent wins. A global period of ignoring events would lose genuine commands.

| Intent/observation | Bridge behavior |
|---|---|
| App Play/Pause | Confirm AVTransport state, forward LMS play/pause once, gate producer independently of cached speaker audio. Queue mode uses native Pause rather than stream's Stop workaround. |
| Natural next-item handoff | Identify the occurrence from TrackURI/ordinal, issue audible STMs exactly once; do not CLI-skip and advance LMS a second time. |
| App Next/Previous | Distinguish requested jump from expected natural join, select the mapped LMS occurrence, reconcile the new core run and cached asset. Previous may restart the current item before selecting the predecessor. |
| App Seek | Fresh per-item RelTime discontinuity/events establish the audible target. Range is only a fetch hint; confirm with GetPositionInfo. No Range-to-LMS-time forwarding on its own. |
| LMS play/pause/skip/seek | Apply the corresponding queue operation or seek to the mapped item, suppress only its matching acknowledgement, then confirm position/identity. |
| LMS playlist edit/power off | Reconcile future items by revision; on off stop production/playback, release owned queue state, invalidate timing and pending work. |

Extend AVTransport parsing to retain CurrentTrack ordinal/URI and available
relative-time events, and GetPositionInfo to return them together with timestamp
bounds. Current `upnp/event_parsers.h`, `upnp/speaker_state.h` and
`upnp/own_speaker_control.cpp:205–304` were built around a stream transport URI;
a constant queue transport URI cannot identify the audible track. Per-field
revision protection is useful but also needs item/epoch fencing. A terminal
rewind to first URI at EOF is not a new repeat unless LMS authorized one.

A canonical item's source time is `source_origin + audible_output_frame / R_out`
for a constant-ratio conversion, with explicit rational source/output maps and
codec delay compensation. For an overlap item, frame zero is incoming-track
source time zero. Full retained assets make speaker seeks independent of network
history. Forward the confirmed LMS time, but reconcile its resulting flush/refetch
as a new decoder run bound to the existing immutable processed asset, rather than
blindly replacing a URL or replaying its prefix. Verify replayed PCM/processing
revision before adopting cached bytes. A settings change needs a new asset.

Progressive forward seeks can create holes that one linear LMS producer cannot
fill concurrently. They require an indexed segment/refill protocol or an explicit
wait/fallback, not invented zero-filled audio. Retained produced prefixes support exact backward seeks in both progressive
and sealed assets; future ranges wait for production. If history is evicted,
route B re-renders through LMS seek as an explicit fallback. Validate CLI seek/refetch,
app seek into buffered data, stale events, simultaneous Next and playlist edits,
and pause while decode is ahead before enabling progressive seeks.

## 5. Queue management and ownership

Mirror the **whole effective LMS playlist**, not a previous/current/next window.
Items not yet prepared have stable occurrence URLs and DIDL metadata from LMS.
Their LMS duration is a display hint; omit speculative byte size until the exact
HTTP promise is known. HTTP never substitutes that hint for exact frame counts.
Use device-proven `AddURIToQueue` appends, with Browse-before/Browse-after revision
and exact-URI ownership fencing plus `NumTracksAdded=1` confirmation;
edit with `ReorderTracksInQueue` and `RemoveTrackRangeFromQueue`. Verify signatures
and returned queue revisions against coordinator SCPD/device behavior before
integration; these actions are not added in phase 1. Retain processed history
independently of the logical queue size.

Respect the approximately 64k speaker limit by mirroring at most **60,000 entries
from the current LMS position**, and log the truncation/current offset. Refresh
that window as needed. Stable occurrence tokens, not moving ordinals, map queue
items to LMS playlist index. A request for an unprepared item, including an app
selection, maps to LMS `playlist index`; prepare and start serving within 15 s.
Distinguish speculative next-item prefetch from an actual arbitrary app selection
using the audible position and pending preparation plan, so normal prefetch does
not issue a destructive skip. Races require generation-fenced request ownership.

LMS remains authoritative for shuffle/repeat and playlist revision. Sonos plays
NORMAL order; repeat visits receive distinct occurrences. Apply LMS edits in
revision-fenced edits, retaining any asset the speaker is still reading.
Dependency edits also reroute a cancelled fade's old tail into the next item.
A foreign queue change while YeneY owns playback invalidates the mutation plan;
never clear new foreign entries based on an old snapshot.

Owner takeover policy: if a nonempty queue is not YeneY-owned, first call SaveQueue
with Title **`Before YeneY YYYY-MM-DD HH:MM`**, verify the saved playlist, then take
over. If saving fails, do not clear the queue. Keep the five newest saved playlists
whose title starts with `Before YeneY`; delete only older playlists with that
prefix, by verified playlist identity and creation order. Never delete unrelated
saved playlists. Handle same-minute title collisions without losing backup identity.
Log saved playlist identity/title and explain restoration through the Sonos app.
A transport URI/length snapshot alone is not a backup.

Hold a coordinator ownership lease and fence every queue update with its current
Browse revision and session identity. Qualify long sequences of single-item appends, removal/reorder around the
playing item, app selections during preparation, backup failure and concurrent
controllers before enabling this behavior. Phase 1 implements none of these
SOAP/controller actions.

## 6. Timing, LMS position and LampaStream

Keep three cursors: decoder/producer, HTTP byte reader, and speaker-audible item
frame. Only the third drives LMS STMs/STMt and light timing. Prefetch, a fast GET
or decoder STMd cannot advance the audible cursor. Maintain per-item rate/frame
maps instead of dividing a mixed-rate global frame counter by the latest rate.

At a confirmed audible item change, restart RelTime coordinates and the model's
phase epoch. Carry a qualified device-rate prior, not the old phase offset.
Between floored position observations advance at unity/prior rate; do not repeat
a frozen start value. Cap the old item at its actual rendered end; any real gap
is represented as a gap. Preserve the existing 1 ms LMS start sentinel only at
its protocol boundary, while internal audible zero remains zero. Repeated reports
must not repeatedly reset LMS interpolation or make it step backwards.

For same-rate tail recovery, the successor asset begins with retained old PCM.
Its source clock and STMs start only after that prefix, at the original incoming
core frame. A different-rate continuation advances the old source clock and
emits no invented LMS occurrence start. The dry-run reports prefix/continuation
assets separately; physical queue insertion and LampaStream analysis pumping
remain later phases.

Crossfade starts the incoming item's LMS clock at zero while its mixed overlap
is audible; its clock counts overlap time once. The outgoing source continues
inside that PCM but is no longer the primary LMS track. Do not add fade duration
to incoming RelTime or subtract it again at overlap completion. Seeking within
the overlap replays its rendered mix unless a new explicit transition plan is
requested; document that retained overlap includes the old track. Manual app
Next/Previous can likewise enter a prefetched mixed intro containing the planned
predecessor tail, even if the skip occurred earlier. For the initial immutable
asset path, preserve that deterministic rendered intro and align LMS to it;
The owner accepts this cached fade-out on manual skip. Strict live manual-skip fade semantics
would need a new transition-specific asset and a controlled handoff, which cannot
undo audio already started by an app action. Do not promise both instantly.

`sonos-position.cpp` retains the single-stream model. The guarded queue controller
reuses `ConnectionPosition` from `position_state.h` with explicit per-item anchors
and speaker observations, independently of HTTP progress. A constant-rate virtual
timeline can later preserve clock phase across proven seamless joins; do not
assume it before validation. Frequent phase resets can leave short tracks unlocked
despite a good rate prior.

The audio tap must publish the actual post-gain, post-mix, post-conversion PCM
heard from the item, including both tracks during overlap. Spooling whole tracks
into the existing small shared-memory audio ring would evict audio before it is
heard. Add a bounded analysis pump from retained items near the audible horizon,
with audio/timing generations switched together at the audible handoff, not at
decode/prefetch. Keep old-item timing valid through its last audible frame.
Invalidate before publishing incompatible rate/phase data. The current v1 timing
contract is one affine active run; review whether item-local remapping suffices
before changing its ABI. Test LampaStream phase, ring retention, group conversion,
seeks and rapid item changes; do not publish false locked precision during relock.

## 7. Crossfade in queue mode

### Route A: yeney-core mixes; primary design

ReplayGain stays in core. Core already implements transition types 1 crossfade,
2 fade in, 3 fade out and 4 fade in/out. `prepareOverlap` at
`third_party/yeney-core/core/player.cpp:757–790` calls `boundary(next)` immediately
before mixing; `outputStep:834–881` applies crossFrame and advances both source
cursors. Split the processed output at that exact callback: item N ends at fade
start; item N+1 begins with the mixed overlap and continues with the incoming
track alone. A gapless queue join reproduces the existing continuous mix without
Sonos doing another fade. Force native CrossfadeMode off for owned route-A
playback, restoring the saved mode on release; coordinator only.

Read LMS player preferences through CLI (escape values correctly; keep the raw
MAC/player ID convention used by this bridge):
`<playerid> playerpref transitionType ?`, `transitionDuration ?` and
`transitionSmart ?`. Read both tracks' album identity and effective sample rates
from the same playlist/settings revision. Smart crossfade skips within the same
album; validate LMS's actual album identity/compilation behavior instead of
comparing display text alone. No core crossfade across differing source sample
rates, even if the later group converter outputs both at 48 kHz. The received
strm transition/period remains authoritative; CLI prediction is advisory and may
race a settings change. Query and test preference semantics against the installed
LMS version before implementation.

Let D_N be decoded source duration and X_(N,N+1) the **resolved** outgoing overlap.
Item N's duration is `D_N - X_(N,N+1)`; its incoming overlap is already counted
inside D_N. For example two 30 s sources with 5 s crossfade give item N 25 s,
then item N+1 starts at incoming source time zero with 5 s of mixed audio. Its
own end depends on its next transition. Clamp the crossfade to actual available
source frames, not approximate decimal durations. Predict X from the preferences,
smart album rule, same-rate eligibility and exact source lengths, including core
period conversion, transition-frame caps and short-track clamping. For an exact
source, fix X as a publication commitment and progressively serve the resulting
promised count. For an unknown-count source, seal after mixer resolution. Exact
HTTP length is known before the first response, even though URL/DIDL placeholders
may already be queued.

Core can cancel for differing rates or insufficient incoming PCM, or shorten the
fade to available frames (`crossWindow` and `prepareOverlap`). The queue planner
must commit the transition before progressive publication and prepare enough
incoming PCM. A later playlist change that disproves it reroutes the remaining
old tail into the next item as specified in section 3. Expose committed/resolved
transition and item-end callbacks; never throw away excess old PCM to satisfy
the earlier length. Unknown-count input is still sealed.

If a published progressive prediction nevertheless proves wrong:

| Policy | Content-Length and audible consequence |
|---|---|
| Pad a too-short rendered item with silence | Honours byte count but inserts a gap, delays incoming source/clock and defeats seamless crossfade. Never silently enable as normal policy. |
| End its response early | Violates HTTP length; may cause retry, skip, stall or app error. Q9 measures this failure; not acceptable normal behavior. |
| Truncate excess PCM to promised length | Honours HTTP length but can cut music/fade and click. Never discard that music silently. |
| Relocate the boundary in retained processed PCM | Owner-selected recovery if no conflicting successor HTTP promise has been committed: keep the promised item end and place the remaining continuous PCM at the next item's start, revising its not-yet-published length. Audio stays continuous, but title/LMS source-zero can move relative to the join. Requires a piecewise source-time map and explicit degraded-transition log. |
| Both adjacent promises already committed | Cannot generally preserve every music frame, both lengths and original timing. Fail the optimization, preserve valid old audio where possible, report the fault, and restart/fall back at a controlled point; a gap or cut cannot be ruled out. |

Thus late boundary relocation is limited, not a universal solution: an outgoing
fade cancelled after N promised a shortened length leaves old-track tail at the
start of N+1, so N+1 source time must remain zero until its incoming audio begins.
Do not report those tail-only frames as advancing incoming LMS time. Such a
nonstandard overlap map must also drive LampaStream. Require audible confirmation
and correct maps before using recovery; otherwise use explicit failure handling.
A user playlist edit after prefetch can create exactly this condition. Immutable
assets and dependency fencing are mandatory.

Fade in (2), fade out (3), and fade in/out (4) alter amplitudes inside the source's
own item without shortening it or creating a cross-item overlap. Preserve their
core envelope and short-track clamping. Crossfade (1) moves the item boundary;
standalone rate mismatch remains a normal unfaded gapless queue join. The group
converter sits **after** mixing, preserving these LMS decisions. Test all types,
smart same-album skips, different albums, insufficient incoming PCM, short tracks,
rate changes, pause/seek inside overlap and preference/playlist races.

### Route B: Sonos native crossfade; backup

Use coordinator AVTransport Get/SetCrossfadeMode (on/off), with saved/restored
state. Supply non-overlapped processed items and disable core crossfade only for
this explicitly selected route, otherwise the music is faded twice. Advantages:
Sonos owns overlap and original item lengths stay unchanged. Disadvantages:
fixed undocumented fade length, no LMS smart/per-period fidelity, possible group
or format limits, and the light tap cannot reproduce Sonos's hidden mix exactly.
Q8 estimates overlap using request and position timing plus the listening note;
HTTP prefetch itself is not overlap. Route B needs explicit owner acceptance of
these losses and measured LMS/LampaStream timing. It is a backup only, not an automatic silent substitute for route A. The owner
accepts cached route-A fade-out on manual skip; native crossfade is not needed
merely to replace that accepted behavior.

### Route C: stream mode during crossfade; last resort

The existing continuous sink reproduces core mixing without per-item promises,
but loses queue metadata/navigation and retains rate-change URI-switch limits.
Use only after route A fails and route B is unavailable or rejected; log why and
retain the LMS settings. Crossfade being enabled is **not** by itself a reason to
exclude a normal eligible track from queue mode. Do not silently turn crossfade
off or alter a global LMS preference.

## 8. Groups and sample rates

LMS synchronization groups stay in stream mode until queue synchronization is
proven. Existing timed pause/skip/start semantics do not establish that separate
Sonos queues stay aligned. Detect LMS sync membership and core synchronization
commands; log `lms-sync-unqualified` and fence any pending queue plan.

Sonos groups share the coordinator queue. Use ZoneGroupTopology subscription
with a fresh topology read on takeover/change, available in `upnp/discovery.cpp`
and `upnp/own_speaker_control.cpp:125–145`. Count visible members, identify the
coordinator UDN and hold one controlling lease per coordinator. Do not let Study
and MBR room processes independently replace the same queue.

**Owner decision deferred:** always-48 kHz, sticky-48 kHz and sticky-current-rate
remain alternatives; none is selected. Until the owner decides and grouped queue
playback is qualified, standalone rooms use native rates and Sonos grouped rooms
stay in stream mode with reason **`group-rate-undecided`**, even when queue is
requested. Joining a group schedules a controlled fallback; never rewrite an
already served item header. Leaving a group can re-enable native queue mode at a
safe new playback boundary after eligibility checks. Coordinator changes fence
pending commands and timing before lease reacquisition. Future conversion must
preserve source-rate crossfade eligibility and exact frame maps; defer converter
implementation until the owner chooses the rate policy.

## 9. Configuration, mode switching and rollout

**Standing owner-test rule:** every future owner device phase must provide a
single unattended command that controls its own test setup, restores changed
settings, and produces one archive with its evidence and verdict. An owner must
not need to operate LMS or copy individual run artifacts. Listening checks are
the only manual steps and must be listed separately from automated qualification.

Phase 2 uses `/usr/local/bin/yeney-queue-suite --server <lms>`: four native ALAC/
FLAC scenarios, crossfade off or 10 seconds, default 4x simulated audibility and
three completed joins. Acknowledged measurement reset after Stop/clear excludes
LMS's automatic reconnect resume; the full log retains that prelude. CLI selection,
preferences and results live in one timestamped archive and
`/tmp/yeney-queue-suite-latest.tar.gz`. Global library queries are reads; only the
fixed Queue Test MAC receives commands. The suite verifies native source codecs
and rates, rather than silently accepting a server conversion. Reports retain
simulated timestamps with additional wall-clock values. LMS elapsed is already
seeded by simulated STMt; speed normalization of its time-display difference is
a diagnostic, while playlist identity remains the verdict check. See
`DEVICE-VERIFICATION.md` for the one-command owner workflow.

Use the **existing per-room systemd environment override**, as described in
README's Settings section; do not overload `/etc/yeney/rooms` or create a second
room configuration database. Phase 3 startup configuration:

```ini
# /etc/systemd/system/yeney@Study.service.d/override.conf
[Service]
Environment=YENEY_PLAYBACK_MODE=stream
Environment=YENEY_QUEUE_DISK_MIB=256
```

| Value | Required behavior |
|---|---|
| `stream` (default) | Select the existing stream sink and transport behavior. |
| `auto` | Accepted and logged as `auto-phase4`; select stream until phase 4. |
| `queue` | Opt in to owned queue playback on a confirmed standalone coordinator. Grouped coordinators and members select stream with `group-rate-undecided`; incomplete topology selects stream with `coordinator-unconfirmed`. |

Reliable length means a resolved exact rendered count, not just an LMS duration
field. Sealing can qualify an otherwise uncertain finite source. Invalid setting
values warn and select `stream`. Empty values are invalid too. Settings are read once at room startup after topology discovery. `YENEY_QUEUE_DISK_MIB` is a positive integer
in MiB, read when the item store is constructed; invalid or overflowing values
fail configuration. It has no effect in stream mode. Phase 3 keeps `stream` as
the default and `queue` as an explicit room opt-in. Any future change to the
default or `auto` behavior is deferred to a separate task.

The mode controller has prepare → active → drain → handoff states. Plan the next
mode before the current audible end, retain the old response/timing until that
end and fence both sinks by generation. Never switch transport simply because
decoding crossed a boundary. An album followed by radio becomes queue → stream;
a finite track after radio becomes stream → queue when its asset is ready.
Radio's unknown end requires an explicit LMS skip/stop/transition intent to
schedule the handoff; do not guess its duration. These automatic return-to-queue
transitions describe future work: phase 3 latches stream for the playback session
and only considers queue again after explicit LMS stop/new play and readiness checks.

A seamless queue-to-queue join is demonstrated. A queue/HTTP-radio transport URI
handoff is **not**: SetAVTransportURI can discard buffered audio and startup can
add silence, as the rate-change investigation showed. Delaying the switch until
the old end preserves the tail but can leave approximately the measured 2.3 s
startup silence. Switching early hides some silence by cutting old audio, which
is unacceptable. Preload/queue-next-URI behavior for a nonfinite HTTP item is an
unproven optional experiment, not this design's guarantee. Without a proven
speaker handoff primitive, no general zero-glitch album→radio promise is possible.
The initial policy preserves the old tail, performs one controlled switch at the
confirmed end, reports any real gap and uses stream for the unqueueable segment.
The owner accepts the album→radio handoff gap: perform the controlled mode
switch and report its measured silence, rather than keeping every mixed playlist
in stream mode solely to avoid that accepted gap.
Do not oscillate sinks on every transient error. A queue fault latches stream
for that playback session; clear on explicit stop/new play after readiness checks.

Log on selection and every transition: room/coordinator, requested setting,
active mode, reason code/text, occurrence, epoch, old/new rate, promised/resolved
frames, lead/cache readiness, pending fallback and measured handoff gap. Suggested
reason codes include `finite-ready`, `live-source`, `length-unverified`,
`lms-sync-unqualified`, `queue-ownership`, `preparation-timeout`, `queue-error`,
`group-rate-undecided` and `crossfade-recovery`. Expose active mode/reason in the
existing room status/diagnostic output and timing diagnostics. Preserve metadata
as track metadata; do not put implementation reasons into Sonos titles. Current
configuration/probe output does not implement this status yet.

Phases and approximate remaining production changes:

| Phase/files | Work and approximate size |
|---|---|
| 1: new item-store/WAV writer + tests | Done in `e3e0c70`. Implemented independent progressive/sealed WAV24 readers, indexed FLAC history, RAM/disk quotas, cancellation/length faults and tests; no playback integration yet. |
| 2: core decoder/player/sink API, item store, new queue sink/dry-run + tests | Done in core `ff2e857`, `478ce6b`, `fbdc8b9`, `9c68473` and bridge `d26bdb8`. Stable occurrences/counts, resolved item boundaries, decode/audible separation, bounded lookahead, worker adoption and tail recovery. Owner-run LMS qualification remains required; no queue playback wiring. |
| 3: `upnp/soap.*`, `own_speaker_control.*`, `event_parsers.*`, `speaker_state.*`, HTTP route | Queue actions/music DIDL, identity/events/position, coordinator lease and finite serving; 350–600 lines. |
| 4: `yeney.cpp`, LMS metadata/control helpers, room status/config docs | Whole-playlist single-item appends/revision-fenced edits, on-demand preparation, SaveQueue/five-backup retention, serialized controls and fallback; 500–850 lines. |
| 5: `position_state.h`, `sonos-position.cpp`, tap/timing exporter | Item clocks, overlap maps, audible analysis pump and generation fencing; 300–550 lines. |
| 6: optional converter and device qualification | Deferred owner rate policy, then exact conversion lengths and ownership/group-change tests; 150–300 lines plus dependency/build work. |

Expect roughly 4–6 engineering weeks plus owner device cycles; seek adoption,
LMS read-ahead behavior and safe live queue edits can expand this. First qualify progressive exact-count and sealed unknown-count assets with
FLAC history, then sink/control integration and committed crossfade/tail routing,
native mixed rates and timing/light publication. Group conversion waits for the
owner rate-policy decision. The final `auto` behavior is the target, not a claim that
all prerequisites are already solved. Stream wire fixtures must remain passing.

## 10. Risks and validation gates

| Risk/open question | Required test and acceptance |
|---|---|
| LMS read-ahead has not yet been qualified on the owner server | Fake SlimProto replay plus real LMS logs: STMs precedes STMd for each occurrence; LMS playing identity matches audible identity without skips. If unavailable, revise the source protocol before claiming seamless queue mode. |
| Transition prediction differs from mixer | Core deterministic PCM tests of all transition types/smart/rates/shortage, verify emitted item frame totals and no missing/duplicated mixed frames; fault injection after prefetch. |
| Exact duration/codec delay errors | PCM/FLAC/MP3/VBR/transcoded fixtures with decoded reference counts; mismatch must never silently trim music or publish mutable headers. |
| Q9 recovery not known | Owner runs optional `--length-mismatch`: short/long WAV and FLAC, records tail/errors/next join. Keep production on exact committed counts or sealed unknown-count assets regardless of a tolerant speaker. |
| Native Sonos crossfade undocumented | Owner runs optional `--native-crossfade` Q8 standalone and grouped; measure bounded overlap and listen. Route B requires explicit acceptance of fixed fade and light-timing limitations. |
| WAV24 Range/pace not yet fully qualified | Real app seek to buffered/unbuffered targets, 1.0x WAV24 delivery, 10 s pause, reconnect, Next/Previous. Require correct RelTime and complete history, not Range-as-seek inference. |
| Cache quota, long tracks, fast concurrent readers | Fake HTTP 1x/9x/multiple ranges, storage full/cancel/slow source; bounded memory, no old-byte overwrite and logged fallback. Measure disk use under several rooms. |
| Foreign queue edits / coordinator races | Two controllers, stale update IDs, topology/coordinator changes, service restart. No clearing foreign entries or simultaneous ownership. |
| LMS/queue loop and seek generation races | Reordered events, CLI commands, cached HTTP seeks and flush/refetch, album edits during mixed overlap. One intended action, correct occurrence and no stale writes. |
| Mixed-rate standalone/group click | Repeat latest clean standalone joins and earlier click case; grouped rooms stay in stream mode until the owner chooses and qualifies a rate policy. Listening plus captured output if available. |
| Timing/light continuity | Replay field clocks plus acoustic/device test: no backwards LMS steps, no frozen start, overlap counted once, SHM retains audible samples and invalidates phase on discontinuities. |
| Mode-switch silence | Album→radio→album and failed queue handoffs, listening plus timestamps. Preserve old tail, explicitly measure gap; do not label an unproved transport switch seamless. |

Owner implementation choice still needed: the Sonos group sample-rate policy
(always-48, sticky-48 or sticky-current-rate). Until then use
`group-rate-undecided` stream fallback. Storage A+D, 8 MiB RAM/256 MiB configurable
disk, whole-playlist mirroring/60,000 cap, automatic named queue backup with five
retained copies, the album→radio gap and cached manual-skip fade-out are owner
choices now recorded above. Route A remains primary and Sonos native crossfade
backup only. Device qualification of preparation deadlines, queue actions,
transition commitment/tail routing and light timing remains required.


### 18 October follow-up qualification

The owner's four ALAC/FLAC runs with crossfade off or the standard 10-second
crossfade reported startup of 0.12–1.04 s, successor leads of 166–274 s and
441000-frame overlap commitments. Their read-ahead results do not establish LMS
identity agreement: CLI echoes encoded the MAC, so all audible polls were
unknown. Responses now decode percent escapes before identity matching and
values decode once. Reports warn above 10% unknown audible polls and fail with
`lms-cli-unavailable` if none succeeds. A short ALAC/off run stopped after track
4 starts must show `lms_matches_audible=true` for every completed occurrence.

A reconnect can resume an earlier playlist automatically. Its headerless FLAC
seek formerly used sealed publication (observed startup 9.15 s). Queue-only
metadata resolution now obtains the first frame's original source position and
STREAMINFO total from a per-session LMS-track-id cache or a metadata-only
`Range: bytes=0-8191` request to port 9000 `/music/<id>/download`. A shared one-second
deadline covers CLI identity and HTTP. Indexed `status` queries distinguish
read-ahead sources from the currently audible track; the first occurrence after
Stop/flush reads the current playlist index to support resumed entries. If the
first FLAC follows another codec, the audible occurrence anchors that index;
uncertain identity keeps sealed publication. Unknown
identity/count or a deadline retains sealed publication with a reason. The
download never supplies decoder input. Stream capability does not use this path.


### Phase 3 implementation checkpoint

Phase 2 is field-qualified by the owner's 18 October unattended suite at bridge
`f4ce585`, core `7f244e5`: four ALAC/FLAC scenarios passed with transitions off
or 10 seconds, LMS identity matched 12/12 completed occurrences, startup was
about one second and successor leads were at least 57 seconds.

**Phase 3b parts 1–8 are complete. Phase 3c device qualification remains pending.**
Explicit `YENEY_PLAYBACK_MODE=queue` now enables owned queue playback on a confirmed
standalone coordinator. `stream` remains the default; `auto` still selects stream
and logs `auto-phase4`. Grouped coordinators and members select stream with
`group-rate-undecided`; incomplete standalone identity selects stream with
`coordinator-unconfirmed`. The phase 3c owner suite is now available as
`/usr/local/bin/yeney-queue-device-suite`; physical qualification still requires
an owner-returned result.

Queue mode prepares finite native-rate assets, mirrors the prepared future window,
backs up and takes ownership of the coordinator queue, and uses speaker-confirmed
identity/position for LMS starts and elapsed time. It supports LMS controls,
future-only playlist edits, mapped Sonos-app controls and echo suppression, cached
history and fresh seek epochs, rendered crossfade intros, and controlled fallback.
It restores CrossfadeMode and removes only owned entries on release/service stop.
Whole-list mirroring, automatic mode selection and the group rate policy remain
open. Local loopback acceptance is not physical-device qualification.

`QueueController` owns one serialized worker and an exclusive in-process room
lease. The host forwards LMS play intent with an epoch, current occurrence and
ordered current/future assets (current plus at least three successors when they
exist). Its asynchronous `play`, `update` and `release` API keeps SOAP away from
core callbacks. Stable shared item handles are watched every 50 ms; sealed
items wait for their actual count, progressive items use their committed count,
and an unpublished predecessor cannot be skipped. Repeated updates do not add
an asset twice, published prefixes cannot be rewritten, and stale epochs cannot
release the current session. HTTP readers never make core audible callbacks.

The queue-only SOAP adapter discovers advertised AVTransport, Queue,
ContentDirectory and topology endpoints on the verified coordinator. Before
mutations it rechecks the standalone lease. Queue and saved-playlist browsing
are paginated and revision checked. Takeover verifies the assigned SaveQueue
identity before any destructive queue operation, retains the five newest
`Before YeneY` backups, and preserves other saved playlists. Ordering uses
creation dates or the timestamp in our backup title, with numeric saved-queue
IDs breaking same-minute ties; unknown ordering fails safely. Same-minute title
collisions get a numbered suffix rather than replacing a saved playlist. Deletion
rechecks saved identity, title and creation order. CrossfadeMode is saved, forced
off and read back; release restores and confirms the saved value.

The finite route and 3a DIDL are used for each published asset. Every append uses
`AddURIToQueue` with `InstanceID=0`, `DesiredFirstTrackNumberEnqueued=0` and
`EnqueueAsNext=0`, between freshly observed and confirming queue snapshots.
`NumTracksAdded=1`, the expected URI list and a changed UpdateID must all agree.
The retained `AddMultipleURIsToQueue` helper is not device-verified: Study S2
86.10-81100 rejected it with UPnP 402 on 20 October 2026. It is never used in
production; batching requires separate device qualification. The queue transport URI is set once, and Play is
sent once, when the first item is ready. Later publications only extend the
queue; Sonos owns the joins. Explicit session-end, mode-change and destructor/service-shutdown
release remove only exact asset URLs registered by this controller, preserving
foreign entries (including URLs that merely copy a session parameter). Release
revokes the epoch for new HTTP readers; existing readers retain their assets.

Loopback tests run the real controller/store/finite HTTP listener against a fake
Sonos coordinator. They cover verified and failed backups, five-backup retention,
name collisions, crossfade confirmation/restoration, progressive/sealed and
not-yet-created assets, current plus three successors, single transport setup,
UpdateID conflicts, grouped-room refusal, stale updates and all three release
paths. The fake reader uses a 40x test clock, 3.5-second prefetch leads, bursts
paced at up to 9x, unaligned and suffix ranges, and reconnects. Separate real
`yeney`/fake-LMS checks now compare unset, explicit stream and deferred auto HELO packets, encoded
FLAC bodies, source HTTP requests and transport SOAP bytes (normalizing only the
fresh per-process session token). These preparation/delivery checks remain
separate from the speaker-evidence acceptance tests below.

Part 3 added speaker-confirmed audible starts behind the then-closed production guard.
The subscription owner forwards SID/coordinator-validated AVTransport events,
with the epoch captured on receipt, to the serialized controller. Queue-field
parsing retains TrackNumber, TrackURI and RelTime without changing stream GENA
parsing. Exact registered URLs, current epoch and increasing notification
sequence fence stale, revoked and foreign observations; a delayed earlier item
cannot become current again. Every candidate is checked with GetPositionInfo,
GetTransportInfo and a second GetPositionInfo. URI, track number and RelTime must
agree across that read bracket and with fields present in the event. Only
PLAYING after the controller's Play can start a new item. TRANSITIONING,
publication, HTTP reads and decoder progress cannot start it. Independent
one-second SOAP polling recovers a missed notification using the same checks.
The start log includes occurrence, speaker track/RelTime and strm-to-audible delay.

Each confirmed item resets ConnectionPosition's phase at its own native rate.
The host supplies a qualified device-rate prior (unity until available); this
prior persists across items, not the preceding item's offset. The worker advances
between floored RelTime readings every 50 ms, never backwards, capped at the
immutable rendered length. Evidence older than two seconds is not projected.
Pause/stop freeze this clock without releasing the queue; resume excludes paused
wall time while retaining within-item phase. Incoming mixed overlap starts at
zero and counts once. CoreQueueSink removes any retained predecessor prefix
before signaling the incoming source and suppresses source starts with no PCM.
Confirmed zero is a distinct core signal, so no fictitious first PCM frame is
needed. The core emits STMs before its held STMd; only the SlimProto boundary
uses the existing 1 ms interpolation sentinel. Elapsed overrides are occurrence
specific, preventing an asynchronous next-item update from resetting the old
item's timer. Speaker pause, resume and stop emit STMp, STMr and STMu respectively;
queue completion is speaker driven, not inferred from decode drain.

Loopback acceptance tests run a real core Player and CoreQueueSink with the real
controller, store and finite listener against fake SOAP/GENA and the phase 2 LMS
song-queue model. They check native 44.1/48 kHz and mixed-rate joins, rendered
10-second crossfades, exactly one STMs per confirmed change followed by STMd,
LMS identity at every join, moving zero-based monotonic/capped elapsed, pause and
resume, STOPPED retaining ownership, missed GENA polling, and stale/foreign/
revoked evidence. Separate deterministic tests verify phase reset/rate-prior
carry and prefix/source-zero safeguards. Room integration remained guarded at that checkpoint;
this does not qualify physical playback or complete LMS control/fallback.

Part 4 adds the guarded LMS control boundary. `Sink::queueCommand` is an
opt-in, nonblocking SlimProto hook; stream sinks retain their existing path.
The host supplies authoritative LMS playlist snapshots with revision and stable
occurrence tokens to `lmsPlaylist`, distinguishes a user selection from decoder
read-ahead, and posts p/u/q/f through `lmsCommand`. Native Pause/Play/Stop target
the coordinator. q/f acknowledge the flush without discarding the owned assets;
only speaker evidence emits playback state. Initial play selects TRACK_NR before
Play. Explicit position/next/previous selects a prepared asset and reconstructs
its successors. Cached mixed intros are retained and logged on manual selection.
A cached backward selection first seeks, confirms its target, then trims its
future, so it cannot remove the still-audible item. Re-added assets receive a new
URL revision when their occurrence already appears in retained queue history.
Core retains bounded track identity for cached revisits, emits one STMs per
confirmed identity change, and does not repeat an already emitted STMd.

`lmsSeek` accepts source milliseconds only from LMS. A target inside the current
asset's produced prefix uses REL_TIME, floored to the SOAP adapter's whole-second
resolution, and reanchors the speaker clock after confirmation. Other targets
request a fresh rendered occurrence through the host preparation callback, use a
new URL and TRACK_NR, and retain the requested source-time origin for LMS elapsed.
The callback must return promptly; an unpublished asset can be filled through
subsequent host updates. `CoreQueueSink::beginRender` excludes the old decoder
run's predecessor tail from an independent seek/history re-render. It does not
change a cached item's committed mixed intro. Missing/evicted history explicitly
requests preparation rather than reading failed storage. HTTP Range never calls
these controls.

Playlist add/delete/move/clear snapshots rebuild only entries after a freshly
confirmed audible URI/ordinal, with UpdateID fencing and read-back verification.
Deleting the audible occurrence or clearing retains its asset until session end;
new future assets follow it. Identical snapshots do not mutate the queue. Every
rebuild logs the LMS revision, occurrence token and retained/cached decision.
The part 3 loopback fake now models Seek/Pause/Stop; the new LMS control suite
uses real SlimProto/core status packets, cached selections, a ten-second pause,
independent re-renders, future edits, and identity checks after each action.
The fixture supplies the authoritative playlist/seek state at the host boundary;
production LMS snapshot acquisition and room runtime wiring remain in part 6,
behind the then-closed production guard. Room queue playback opened later in part 7.

Part 5 adds app reconciliation on the serialized controller worker. A host binds
`ControllerOptions::lmsControl` to `controlLms(host, rawMac, command)` (port 9090
by default), and `fallbackRequest(reason)` to the part 6 fallback boundary.
The CLI adapter sends a raw MAC, decodes percent-encoded replies, requires an
exact acknowledgement, and makes one attempt with a one-second socket deadline.
No production room is connected to these callbacks yet.

Before interpreting a position/state observation, the controller verifies the
standalone lease, queue URI list **and UpdateID**, selected queue source, NORMAL
play mode and disabled CrossfadeMode. It brackets GetTransportInfo with matching
GetPositionInfo readings and rejects GENA disagreement or stale SID sequences.
Own target/seek fences and expected transport states acknowledge YeneY commands;
matching LMS echoes consume separate state, occurrence-token and source-time
expectations (five-second deadlines). A correlated retained decoder run consumes
one ordered `strm f`/`s` pair, fenced by the audible occurrence; the host retains
the asset instead of decoding that echo. Fresh seeks authorize a one-shot core
`takeQueueRefetch` before starting the new decoder. Only a speaker-driven,
decode-ahead sink can cancel an unfinished producer without flushing prepared
assets or clearing track/audible history. Its incomplete old asset is invalidated
and cannot be mistaken for reusable complete history. Default sinks keep their
existing destructive-start behavior. Preparation has a 15-second publication
deadline, followed by the five-second transport acknowledgement deadline. A fresh, SOAP-confirmed contrary app event
supersedes an older pending LMS intent. An unconfirmed own intent expires into
foreign-control rather than waiting forever.

Confirmed app pause/play forwards `pause 1`/`play`; an adjacent prepared owned
item maps its stable token through the authoritative playlist to `playlist index`.
A successor at the rendered end (1.5-second RelTime quantization allowance) is a
natural handoff and does not send an extra CLI skip. Other non-adjacent or unmapped
jumps release ownership. Within-item RelTime discontinuities over 1.5 seconds,
or any changed position while paused, forward `time` with the asset's source
origin. The produced-history check retains the existing URL and reanchors the
audible clock for cached seeks, including backward seeks. A target within the
item but beyond its produced prefix uses the part 4 nonblocking preparation
boundary, a new occurrence/URL and confirmed TRACK_NR; the old item's reported
seek time is never used as evidence for that new asset. HTTP Range
never participates. Echoed playlist selections still reconcile simultaneous LMS
playlist edits, without seeking the speaker again.

Queue edits, revision-only changes, mode/source/topology changes, foreign items,
unmapped stop/jumps, seeks outside the item and CLI acknowledgement
failures latch one `foreign-control` request per epoch. Release refreshes queue
identity and UpdateID for every exact-owned removal and restores CrossfadeMode.
After a grouping change only these cleanup actions may address the originally
verified device without the playback lease; no transport playback command gains
that exception. Cleanup failure is logged, URLs are revoked, and no retry loop
runs. Part 6 implements the requested fallback; this part only calls the boundary.

`tests/queue_app_control_test.py` uses the part 3 real core/SlimProto fixture,
loopback Sonos SOAP/GENA and a loopback CLI server returning encoded replies.
It checks app controls, history seeks, identity and echo counts, own commands,
rapid contrary actions, a held four-second prefix followed by a fresh seek,
and each foreign-action category with exact preservation
of foreign entries and one release/request. The fixture joins its core thread
before destroying the controller so late command echoes cannot outlive it.

Parts 7–8 complete the real-binary acceptance and documentation below. The
unattended physical device suite follows in phase 3c. Agents never run against
real LMS or speakers or deploy on LXC 113.


### Phase 3b part 6: controlled fallback and guarded room runtime

`RoomMode` owns `prepare → active → drain → handoff → active(stream)`.
Preparation has a 15-second deadline starting at LMS play, not idle service startup. Foreign control, preparation timeout,
SOAP error, length/storage fault and coordinator/group changes each latch one
fallback per playback session. A valid current finite item drains at the speaker,
with only its future entries removed. Projected position and HTTP progress cannot
end the drain: confirmed RelTime/end or transport stop is required. An unusable
current asset or foreign control stops only an exact owned URI, confirms Stop,
and preserves the last confirmed LMS source position. A failed/unconfirmed Stop
waits for confirmed end, without repeated Stop or an early URI switch. Group
lease loss never grants playback commands to the old coordinator; its restricted
cleanup policy still permits exact-owned removals and CrossfadeMode restoration.

Cleanup precedes selecting the existing `CoreSonosSink`/stream transport. Queue
URLs are revoked even if cleanup fails. Open immutable HTTP bodies may finish;
they cannot generate speaker evidence or change the new sink. `ModeSink` fences
PCM/descriptors with a generation. The core's opt-in generation change cancels
its old HTTP producer, decoder, queued PCM and audible callbacks, flushes the
selected sink and publishes its new sample-rate capability before readiness.
The room then issues one raw-MAC CLI continuation at the confirmed playlist
position/source time (or the successor at a completed finite boundary). No CLI
retry loop runs. The gap is measured from the confirmed queue endpoint to the
first confirmed playing stream URI; it includes the one-second evidence poll
resolution. A stream session remains latched until explicit LMS `q`, a new `s`,
and a fresh standalone-coordinator readiness check.

`yeney.cpp` constructs `QueueRoom` only for an eligible queue selection. It owns
the finite HTTP route/controller, queue sink, stream sink and mode router. Its
worker acquires production CLI playlist snapshots, validates their complete
count/index, decodes fields and preserves occurrence tokens across edits. Missing
future assets remain unpublished placeholders. Duplicate edits whose identity
cannot be resolved fail closed. Metadata/CLI/SOAP work stays off the audio thread.
Part 6 shipped with `queuePlaybackReady` closed and selected the original stream
client with `queue-unavailable`. Part 7 opens it for explicit standalone `queue`
after real-binary loopback acceptance. Default stream sinks use generation zero and never
enter the core handoff branch. HTTP constants, stream URLs and stream transport
ordering are unchanged.

Room diagnostics and the existing pacing JSON expose requested/active mode,
state/reason, occurrence/epoch/generation, ownership, mirrored successors, last
fallback, latch and handoff gap. Selection and transitions log room/coordinator,
mode/reason, occurrence/epoch, old/new rate, promised/actual frames and gap. The
stream rate remains unknown until confirmed rather than assuming a source rate.

`room-mode-test` covers all triggers, readiness/session latch, blocked Stop,
generation fencing, snapshot tokens and JSON status. `tests/queue_mode_test.py`
uses the real controller, finite HTTP/store and loopback SOAP/CLI to verify one
release/handoff per reason, an intact tail, index/time continuation, URL revocation
and late reads/events. Core speaker tests exercise generation reset, capability
publication and subsequent stream playback without duplicate audible starts.
Existing app/LMS control and legacy wall-mode wire fixtures remain required.
The real-binary acceptance below complements these boundary tests.


Part 6 validation also requires `LAMPASTREAM_CHECKOUT=/path/to/LampaStream make test`.
The checkout must have origin `ThaYapeMan/LampaStream` and stays read-only;
`PYTHONDONTWRITEBYTECODE=1` prevents import cache writes there. The real SHM
consumer test uses `require_v1=True`, and must print its source path and
`PASS cross-repo LampaStream SHM consumer (require_v1=True)`. A missing explicit
checkout or import dependency fails rather than skipping. Dependencies, if
needed, belong in a virtual environment under `/tmp`, never in the checkout or
system Python. A skipped consumer is not an accepted validation result.


### Phase 3b parts 7–8: real-binary acceptance and shipped limits

`tests/queue_binary_test.py` launches the real `yeney` application, using the
production room/controller/sink wiring. The fake coordinator exposes SOAP and
GENA, and its HTTP consumer prefetches 3–4 seconds before joins in bursts paced
at up to 9x, with Range reads and reconnects. The fake LMS uses SlimProto and
CLI port 9090 with raw MAC requests and URL-encoded IDs/replies. No fixture host
stands in for the application. Acceptance first passed with the readiness guard
opened only by a temporary test build flag, then passed again after opening the
production guard with no test flag, including the Sonos-clock pacing default.

Coverage includes ALAC and FLAC albums with at least three joins each, a 10-second
crossfade across album boundaries, mixed 44.1/48 kHz, verified Before YeneY backup,
CrossfadeMode off/restoration, prepared successors, LMS/audible identity, STMs
before STMd, per-item monotonic elapsed, a 10-second pause/resume, next/previous,
history and fresh-asset seeks, add/delete/move/clear, app controls and echo
suppression. Foreign queue edits exercise one real stream handoff at the confirmed
point, preserved foreign entries and LMS index/time continuation. Shutdown removes
owned entries and restores CrossfadeMode. Startup tests verify grouped-room stream
selection, while unset/stream/auto have identical HELO, encoded stream bodies,
source requests and SOAP transport order/bytes (only the session token normalized).

The acceptance exposed and fixed a pending-to-published handle race, source-time
carryover after a cached app seek, and same-token re-render ordering. Unpublished
superseded handles wait for a refreshed snapshot; prepared seek handles refresh
without removing the target. Only the final seek command requests fresh decoding,
and its flush echo cannot cancel the prepared target. A deterministic publication
race test leaves the stale handle in place across several worker ticks.

Fallback reasons are:

| Reason | Trigger |
|---|---|
| `foreign-control` | Unmapped app action, foreign queue/source/item/mode edit, or ambiguous identity. |
| `preparation-timeout` | Finite preparation or command confirmation cannot complete in its bounded deadline. |
| `soap-error` | Coordinator RPC/transport failure. |
| `length-fault` | Promised rendered count cannot be fulfilled. |
| `storage-fault` | Asset storage/history failure or exhausted protected storage budget. |
| `coordinator-change` | Loss of the verified coordinator/grouping lease. |
| `lms-unavailable` | A verified LMS contradiction after reconciliation, or 30 s without a successful reply during a pending operation. |

Fallback preserves a valid audible tail where possible; foreign control freezes
at a speaker-confirmed point. It releases owned entries, restores CrossfadeMode,
fences generations and hands off once to stream at the same LMS position. The
stream latch lasts until explicit stop followed by new play and readiness checks;
there is no automatic reacquisition or retry loop. Handoff is measured, not
claimed gapless. Unknown/unusable assets cannot promise an uninterrupted tail.

Room diagnostics and per-room pacing JSON expose `requested`, `active`, `state`,
`reason`, `occurrence`, `epoch`, `generation`, `owned`, `mirrored_ahead`,
`last_fallback`, `handoff_gap_ms` and `latched` in the playback status. A gap of
`-1` means no confirmed handoff measurement yet. Transition logs also include
room/coordinator, old/new rates and promised/actual frames.

Production limits remain standalone rooms only, explicit queue opt-in, and a
bounded prepared window/history budget. Whole-list mirroring, `auto` selection
and the group rate policy remain open. Phase 3c will provide unattended physical
device qualification; this completion records loopback acceptance only.

### Phase 3c: unattended physical-device runner

`/usr/local/bin/yeney-queue-device-suite` controls only a verified standalone room
and its raw-MAC LMS player. It saves the full playlist/state/preferences, verifies
a named Before YeneY backup and its ordered contents, uses a room-only queue
drop-in, and runs real-time ALAC/FLAC album joins, standard 10 s crossfade, optional
44.1/48 kHz joins, LMS controls and stop/play re-arming. SOAP, LMS and process-fresh
room JSON are sampled at 1 Hz; strict queue/identity/start/seek/continuity criteria
and service-stop release checks produce a separate physical qualification verdict.
The implementation remains unqualified on hardware until the owner reports it.

A standard-library TLS WebSocket sampler polls only getPlaybackStatus at 5 Hz,
with a 60 s reference after removing the queue drop-in in normal stream mode.
Per-mode presence, quantization, RTT, continuous per-item fit residuals and
RelTime start differences are archived. Errors and inflated error payloads are
reported unavailable and cannot fail device qualification or alter production
clocking. Sonos-app and acoustic checks remain optional human observations.

The runner holds compatible suite locks, restores on SIGINT/SIGHUP/SIGTERM/error,
retains timestamped/latest archives and verifies the original room environment
and LMS state. The documented systemd-run invocation survives an SSH disconnect.
Loopback CLI/SlimProto/SOAP/systemd/status and self-signed TLS WebSocket tests
exercise these transactions; they are not an owner device run. See
[device verification](../../DEVICE-VERIFICATION.md#phase-3c-unattended-queue-playback-device-suite)
for installation, the full command paths and the Proxmox archive pull.

### Slow LMS hardening

Queue app actions, full playlist snapshots, render refetch, fallback continuation
and rearm now use the queue-only LMS client. Core/audio callbacks enqueue work;
app commands use a dedicated CLI worker and snapshots use the room worker. Each
request owns a fresh socket. Short queries have a 6 s deadline and one read retry;
full snapshots and mutations have 30 s. A mutation is never blindly resent, even
after a lost acknowledgement: mode/index/time queries reconcile its outcome.

A missing reply preserves pending intent. Only sustained lack of any successful
reply for 30 s, or a concrete contradictory verification after that reconciliation
window, requests `lms-unavailable`. CLI wait time is excluded from preparation
and pending CLI work holds only LMS-dependent confirmation/echo leases. Pending
CLI jobs/completions are generation-fenced on fallback. Slow (>1 s) replies are
logged at most once per command per minute; queue JSON exposes
`lms_cli_slow_replies` and `lms_cli_failed_replies`. Default stream mode retains
its old wire/transport path and status serialization.

### Reconnect-loop prevention (20 October 2026)

Queue rearm and fallback generations are internal fences, never LMS reconnects:
core resets the old producer/decoder and signals readiness without another HELO.
An already preparing/playing session ignores a rearm request until an explicit
LMS Stop followed by Play; ordinary repeated strm s remains track selection.
The room mode controller also limits rearm generation changes to three in a
rolling five-second window (initial bootstrap excluded). A fourth request logs
`rearm-loop` and the last 16 SlimProto command letters, then requests one latched
stream fallback with reason `rearm-loop`. This defensive block lasts until the
room service restarts, so repeated q/s cannot clear it. The usual finite-tail and
generation-fencing handoff applies.

The shared fake LMS models same-connection HELO as q/s of the current song. The
real-binary field replay checks one takeover, one enqueue, one Play and one HELO,
then sends a repeated same-track start and checks that no rearm occurred.

The rearm CLI index continuation explicitly admits its f/s into the decoder;
without that admission, the manual-selection path could swallow a new session's
source before any prepared asset existed. Identical valid queue strm s retries
retain their current producer while no STMd-requested successor is outstanding;
STMd credits admit new tracks even when LMS reuses the exact HTTP request.
Stop/flush, source/format changes and new generations still permit a fresh start.

### LMS protocol discipline (20 October start-loop correction)

Queue playback is a SlimProto receiver. LMS owns source selection and its
StreamingController song queue. An incoming command never causes a CLI mutation.
In particular, `q` followed by `s` adopts that track in the existing queue
preparation/generation. A cached selection or a genuine stream-to-queue rearm
resumes the already received HTTP start internally; readiness never sends
`playlist index`. The rearm-loop circuit breaker remains a last-resort guard.

| Evidence/state | YeneY response | LMS StreamingController meaning |
|---|---|---|
| New TCP connection | Exactly one HELO; no HELO for an epoch/readiness change | `playerReconnect` may invoke `_Continue`, restarting a source; never use it as an internal reset |
| `strm q`, including repeated idle stops | Acknowledge STMf and confirm speaker Stop; keep queue ownership, without unsolicited STMu | STMu is an underrun, not the acknowledgement of an LMS-issued stop. Stop/flush before a selection is normal; it does not authorize YeneY to select an LMS track |
| `strm s` while idle/stopped/preparing | Accept/adopt its producer or cached selection, with no CLI command | LMS has already selected and streamed the song |
| `strm f` canceling a speculative future producer | Acknowledge flush; discard an unmapped deleted successor and retain the audible transport | LMS can cancel a removed prefetched song without stopping its playing song |
| Cached selection | STMc, actual HTTP/RESP handshake, discard encoded body and retain existing PCM; no decoder reopen | LMS clears its connecting state only after RESP |
| `u` after autostart-zero buffering | Release producer admission; keep audible state speaker-confirmed and suppress a redundant Sonos Play | BufferReady/start synchronization is distinct from unpausing |
| Speaker confirms the owned item is playing | STMs once for that start | Started: LMS playing identity becomes that audible song |
| Fully decoded local item, STMs already sent | STMd no earlier than the next event-loop round | StreamingComplete: `_NextIfMore` admits a successor only when the song queue has fewer than two songs |
| Fully decoded remote item, STMs already sent | Hold STMd until remaining speaker-audible time is strictly below `YENEY_QUEUE_NEXT_DELAY_S` (default 20 s, 1–120 s) | Admit a remote successor near playback, avoiding an idle service connection; never use HTTP reads as the playback clock |
| Fault with zero audio body bytes | STMn; no fabricated STMs | StreamingFailed: `_StopNextIfMore` may stop and select another song |
| Fault after any audio body bytes | Report the queue fault and preserve the produced tail; no STMn | Received audio must not be misrepresented as an empty failed source; the ownership/fallback policy handles it |
| Unambiguous Sonos user action | One CLI mutation with `reason=sonos-user-action`, then verify by queries and suppress its SlimProto echo | Explicit external user control, not a response to LMS's own messages |
| Fallback handoff | At most one query-verified continuation batch with `reason=fallback`; its q/s cannot rearm queue mode | Resume the selected position in the latched stream session without a feedback loop |

`LmsClient::submit` is the queue mutation choke point. Missing/internal reasons,
unrecognized mutations and attempts to mutate through the query API are rejected
before network I/O. All mutations log their reason and command; no mutation is
blindly retried. Remote-source classification uses LMS's remote flag, negative
IDs or a non-file URL. Unresolved identity conservatively delays successor
admission until the prepared item has metadata.

These independently implemented rules were studied from
[LMS StreamingController](https://github.com/LMS-Community/slimserver/blob/public/9.1/Slim/Player/StreamingController.pm)
and [LMS-uPnP's squeezelite state machine](https://github.com/philippe44/LMS-uPnP/blob/master/application/squeezelite/slimproto.c).
No implementation from those projects is included in YeneY.

LMS can expose the previous `time` while waiting for the new run. The room therefore
uses a read-only CLI `listen 1` subscription, filtered to its decoded player ID,
to retain an authoritative `time` command target. It never derives a seek from
HTTP Range, and never sends a setter in response. Owned seek notifications are
suppressed as echoes. STAT elapsed is relative to the new run; room status and
handoff evidence retain absolute source time, while LMS adds its own seek offset.

Expected re-render cancellation removes an unrendered producer's descriptor and
its asset from the future plan. Reservations read the core's actual occurrence
counter without advancing it. A superseded future producer waits for real LMS
audio, without being mistaken for a SOAP fault or requesting another source.
Service-shutdown flushes cannot request a fallback continuation.


### Real LMS regression acceptance (20 October 2026)

`tests/real_lms_test.py` runs the production binary against official LMS 9.1.1
and the loopback Sonos coordinator. The temporary distribution, library, cache,
logs and optional private Perl SSL modules stay under `/tmp`; nothing is installed
system-wide. The test records every SlimProto packet and bridge CLI request.
Generated 24-second FLAC files exercise three natural joins and 44.1/48 kHz;
ALAC library input at both rates uses LMS's default FLAC transcode. A local HTTP FLAC source
exercises the remote near-end completion gate. Valid FLAC padding makes the tiny
constant-PCM fixture large enough for LMS's remote scanner without changing PCM.

The acceptance checks LMS controls, seek, playlist changes and stop/new play,
one HELO on the connection, matching audible identity, one initial takeover,
no queue-generated CLI mutation and service-stop cleanup. Source formats are
also exercised natively by the existing fake-LMS decoder tests. This is protocol
acceptance with a fake speaker, not device or listening qualification.

Rapid q/s selections retain their latest LMS occurrence token separately from
asynchronous status evidence. This prevents a previous/next selection from
being treated as an in-track seek while the prior confirmation is still queued.
A confirmed pause likewise remains paused internally until the speaker confirms
resume; autostart-zero producer admission alone never fabricates STMr.

Future-only playlist reconciliation is deferred while an owned Seek is pending;
the next snapshot retries after confirmation instead of weakening the audible
position check. Loopback LMS echoes include q/s and producer admission u, as
observed with the real server.
