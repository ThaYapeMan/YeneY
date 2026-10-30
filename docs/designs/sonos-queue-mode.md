# LMS-driven Sonos queue relay

The queue-mode base is a thin relay. LMS is the sole playback master. Queue
mode is the built-in default. Explicit `stream` preserves continuous transport,
URLs and wire fixtures. Queue selects source-aware playback for a standalone room
or the coordinator of a multi-speaker group: local files and finite non-live remote items use the relay; live and unknown-duration sources use the existing continuous stream path.
`auto` and invalid values still use stream with an effective-mode log. Existing
queue overrides are redundant but harmless and are never removed by installation.
There is no automatic fallback inside this queue release.

The previous advanced implementation is preserved by immutable annotated
`queue-full-v1` tags in both repositories (bridge `98727db`, core `13db8b1`).
[Parked features and rebuild checklist](queue-parked-features.md) gives the
files, commits, tests, device evidence and future rebuild order. It replaces
all earlier claims of advanced controller/device qualification.

## Per-track source/path selection

Classification runs on the room worker at each LMS `strm s`, before opening
its audio connection. A read-only CLI snapshot supplies the track admitted by
LMS. Explicit local files select relay. Remote items (remote flag, negative id
or non-file URL) also select relay when LMS supplies a positive finite duration
and no nonnegative live edge. Live or unknown-duration metadata selects stream.
Normal selection, readiness and class boundaries cause no LMS CLI mutation or HELO.
A confirmed room refusal is the exception: one reason-checked LMS stop.

| Play/source | Active path | Boundary behaviour |
|---|---|---|
| New local file after q/idle | relay / local-source | Existing finite WAV takeover and startup |
| New finite non-live remote | relay / remote-finite | Progressive upper-bound WAV; overflow continuations preserve audio |
| New live or unknown-duration source | stream / remote-source | Existing stream metadata, artwork, PCM feeder, HTTP and pacing; no queue acquisition |
| Next track has the same class | Current path | Existing append/continuous behaviour |
| Relay → live/unknown | Keep relay until confirmed final-item end | Stop/release owned entries and restore crossfade; fresh continuous start |
| Stream → local/finite remote | Keep stream until submitted audio is audibly drained | Close the continuous session and Stop; fresh relay takeover/start |
| Endless remote | stream / remote-source | Starts progressively; no finite length or end is required |

A deferred class-changing start retains the original SlimProto request without
connecting its audio source early. It suppresses the old path's final STMu
while the already-admitted successor waits. An internal playback-buffer reset
at drain clears old boundaries without reconnecting. Logs record `path-pending`,
selection reason and `path-handoff ... gap_ms` from confirmed end/drain to
confirmed PLAYING on the newly selected URI. A short class-change gap is
accepted; a live track is never cut merely to change class. Explicit LMS q or
selection cancels pending work. The ordinary stream-only room is unchanged. At a class boundary its existing
metadata formatter queries the selected playlist index rather than racing LMS's
update of the previous audible index; ordinary stream queries remain unchanged.

## State table

One serialized worker owns SOAP and an ordered list of immutable WAV assets,
each identified by the core's monotonically allocated LMS stream occurrence.
The only player states are **STOPPED**, **PLAYING**, **PAUSED**. Pending render
records are data, not a separate player state. No playback epochs, generations,
rearms, intent guessing or readiness continuation exist in the relay. The
CLI mutations report unambiguous speaker-originated stop/pause/resume or stop
a room after a confirmed policy/transport fault. A fixed legacy route namespace keeps the existing WAV URL shape;
it is not a playback generation and never causes a capability announcement.

| Input/evidence | Renderer work | SlimProto result / next state |
|---|---|---|
| Genuine LMS connection | None | Exactly one HELO on that connection |
| LMS `strm q` | Stop, remove exact owned URIs, restore CrossfadeMode | Normal core producer reset, STMf, STOPPED |
| Local LMS `strm s` following q or idle | Render the selected source/offset; publish once exact length is known; acquire queue; AddURIToQueue; select `x-rincon-queue:<UDN>#0`; Seek TRACK_NR 1; Play once | Normal stream connection acknowledgements; remain STOPPED until speaker confirms playback |
| Local successor `strm s` after our STMd | Render and append with AddURIToQueue; never issue another Play for the successor | Await its confirmed audible start |
| GetPositionInfo identifies a prepared owned item and GetTransportInfo says PLAYING | Feed confirmed item/frame position to the core and synchronously publish status | STMs once for that stream, PLAYING |
| Current item fully produced | None | STMd in a later core round than STMs; local successors may be requested early |
| LMS `strm p` / `strm u` | Pause / Play (resume only when paused) | Speaker-confirmed STMp/STMr, PAUSED/PLAYING |
| Speaker STOPPED at the natural end of the final owned item | Feed confirmed final state to core | STMu once, STOPPED |
| LMS seek (q+s with LMS source offset) | Same as a fresh play; no independent CLI/REL_TIME interpretation | New rendered stream, then confirmed STMs |
| Source/decode/length failure | Log fault; Stop/release | First consecutive fault: STMn; second within 10 s: one LMS stop, no playlist walk |
| Room/topology/ownership/SOAP/transport fault | Log reason; Stop/release when still coordinator | One LMS stop, no STMn or automatic advancement |
| New track while member of another group | No transport writes on either path | One logged LMS stop, STOPPED |
| External source change/group membership, even during the own-command window | Release local ownership without speaker writes | One raw-MAC LMS stop, STOPPED |
| External stop on our queue, outside the 3 s own-command window, before the final 2 s | Release exact owned items and restore crossfade without Stop or URI switch | One raw-MAC LMS stop, STOPPED |
| External pause/resume, outside the own-command window | Report confirmed state; suppress the resulting LMS echo | One LMS pause 1 / pause 0, PAUSED/PLAYING |
| Service shutdown | Stop and release exact owned entries; restore crossfade | Process exits |

These messages let LMS's StreamingController own song advancement: STMs reports
TrackStarted, STMd reports decoder completion / NextIfMore, STMu reports output
underrun/end, and STMn reports a failed source. Room failures never use it. YeneY never responds
to LMS's own starts by jumping the LMS playlist. Read-only status queries supply
metadata; requests use the raw player MAC and replies are CLI-decoded once.

The optional core Sink generation API remains source-compatible, but its old
queue generation state/reset is removed. Default stream/WAV/SHM sinks do not
consume renderer-failure notifications. Core-rendered transitions, exact sample
counts, FLAC seek origins, finite container decoding and speaker clocks remain.

## Queue ownership and finite transport

On first acquisition foreign Q:0 entries are saved in an identity-verified
`Before YeneY <timestamp>` playlist, then removed with UpdateID fencing.
An empty or exclusively owned queue needs no backup. Repeated admissions reuse a
verified backup only for the same foreign transport and identical queue contents;
a missing or changed saved-queue identity requires a new backup. An unchanged
queue left after a successful switch and later admission failure also reuses
that backup. For a foreign
transport, admission switches to this room's own queue and confirms CurrentURI
before reading or changing transport settings. A 7xx setting refusal before
the first switch retries once after a confirmed switch; a post-switch refusal
remains a room fault. This also takes a coordinator's
whole group away from a live Spotify Connect session. An already selected own
queue retains its existing settings/enqueue/select/seek/play order. CrossfadeMode is read,
forced off and verified; release restores it only while the transport remains ours. Removal refreshes Browse and
removes only exact owned URIs from the back, preserving foreign insertions.
Every append uses Browse-before/after fencing and requires NumTracksAdded=1.

Production uses only **AddURIToQueue** with InstanceID=0 and EnqueueAsNext=0.
Normal appends use DesiredFirstTrackNumberEnqueued=0; a continuation inserted
before a queued successor supplies its verified 1-based insertion position. The retained unit-tested
The removed AddMultipleURIsToQueue helper was **not device-verified**: Study returned 402 on
20 October 2026. Its rejection is unexplained and may have been the invalid
U+0016 album XML identified the next morning. It is not a production path.

The finite item store, progressive exact-length/sealed WAV publication, HTTP
Range/reconnect support, metadata DIDL, XML 1.0 sanitiser and shared artwork
resolver are unchanged. Metadata repair is logged by field/count, once per
sent item; the sanitised plain title is sent consistently.
Sonos crossfade is off; LMS transitions are still rendered by the core. Queue
crossfade and mixed-rate owner qualification are parked, not declared passed.

`YENEY_QUEUE_NEXT_DELAY_S` defaults to 20 (valid 1–120). The core retains
its finite-remote successor gate for other queue hosts; production remote
sources with a positive non-live duration now use upper-bound queue promises. `YENEY_QUEUE_DISK_MIB` remains a
per-room disk quota (default 256). Unknown local exact length waits for sealing. Estimated remote declarations use
the explicit upper-bound kind below; HTTP Range offsets never determine length.

## Status and extension points

The suite reads the existing per-room `stream-pacing-<room-hex>.json` file's
`playback` object. Queue fields: requested/active=`queue`, path=`relay`/`stream`, reason=`local-source`/`remote-finite`/`remote-source`,
state, occurrence, owned, owned_item_uris, current_owned_item, last_fault.
Each observed state/ownership/item change publishes an atomic complete file on
the same worker call, without waiting for the periodic stream metrics tick.
Speaker polling is normally every 500 ms, with 100 ms start/end windows; publication cannot precede an observation.
Stream diagnostic representation stays unchanged in stream-only rooms. The queue room
keeps requested/active=`queue` and exposes its per-play path separately.

Future components attach to these boundaries, without changing the relay state
table: (2) a separate failure/release observer and stream sink selector for
fallback; (3) a speaker-evidence observer with explicit own-command matching for
app actions; (4) a mirror supplying append/remove plans for future items; (5) a
room eligibility/group policy outside the relay. Per-source path selection is
now implemented, but automatic recovery from failures and auto selection
remain separate future work; coordinator playback is now enabled. None may fabricate an LMS start
or mutate LMS in response to LMS's own command. Tests for a future component
must retain the base relay acceptance and stream wire fixtures.

## Qualification

Study on 21 October 2026 proved the primitive path: sanitised finite 24-bit WAV,
AddURIToQueue, queue URI, Seek and Play, audible in 1.29–1.62 seconds. The old
suite's prepare/owned=false status lag was a controller-status failure, not an
audio failure. These observations do not qualify the new relay on hardware.

Agent tests use only loopback: one strict Sonos SOAP fake, real yeney, a replay
SlimProto peer and temporary real LMS 9.1.1. The owner-only relay suite uses
`/usr/local/bin/yeney-queue-device-suite`; see
[device verification](../../DEVICE-VERIFICATION.md#phase-3c-unattended-queue-playback-device-suite).
Physical relay acceptance remains pending an owner run.

## Size of this rebuild

Counts include blank/comment lines and exclude tests and scripts. The bridge
scope is tracked `queue*.{h,cpp}`, `core_queue_sink.*`, `upnp/queue*`,
`playback_mode.h`, old `room_mode.*`/`mode_sink.h`, and new `room_status.h`.
This deliberately includes the unchanged store, HTTP route and dry-run tools.
Core scope is `core/queue*.{h,cpp}`; shared Sink gains two default no-op hooks.

| Scope | queue-full-v1 | Relay |
|---|---:|---:|
| Bridge queue C++ including retained primitives/tools | 5882 | 3601 |
| Bridge control/metadata layer (room/controller/mode/client/playlist headers and sources) | 2833 | 535 |
| Core queue-only modules (rendering retained) | 753 | 752 |

Conceptual reference studied, never copied:
[LMS-uPnP SlimProto control](https://github.com/philippe44/LMS-uPnP/blob/master/application/squeezelite/slimproto.c)
and [renderer callbacks](https://github.com/philippe44/LMS-uPnP/blob/master/application/squeeze2upnp/squeeze2upnp.c).
The original YeneY implementation and tests retain their existing license.

### Speaker sample-rate limit

Modern queue-configured rooms advertise 192 kHz. Native local FLAC/ALAC
is converted in yeney-core before ReplayGain, volume and crossfade: 88.2/176.4
to 44.1 kHz; 96/192 to 48 kHz. Exact ceil frame counts permit publication
before DSCO. Unknown source counts retain sealed publication. Live/unknown-duration remote sources
stay on the stream path, with the same converter for high-rate input.
Rates ≤48 kHz pass through. Unsupported higher families fail with STMn.
Ordinary stream rooms retain their existing 48 kHz HELO and LMS/SoX conversion;
legacy rooms retain 44.1 kHz advertisement and LMS conversion.
See [filter design and measurements](queue-hires-decimation.md).

### Speaker observation and failures

The relay alone schedules queue position/transport queries: a 500 ms steady
interval (`YENEY_QUEUE_POLL_MS`, 100–2000), 100 ms within three seconds after
Play/Seek/track changes and within three seconds of item end. A seek is an
LMS q+s/new Play. GENA triggers immediate refresh. The stream display does
not issue a second set of queue queries. Successful polls are silent; changes
of state, current item and TransportStatus are logged. Only PLAYING confirms
an audible item. A non-OK TransportStatus or STOPPED for more than three
seconds after Play faults once, stops/releases owned entries and reports one LMS stop without STMn.
Queue status includes `rendered_rate`; the owner suite verifies both high-rate
families where the library contains suitable tracks.

### Speaker-originated control (23 October 2026)

TransportState, TransportStatus, CurrentTrack, CurrentTrackURI, NumberOfTracks
and AVTransportURI changes are logged with old/new values from GENA and polls;
unchanged polls are silent. Relay state/current-item publication also logs one
change line. GetMediaInfo supplies transport URI and count alongside position
and transport polls. An empty CurrentTrackURI is meaningful stop evidence.

Own transport commands suppress external classification for three seconds;
SlimProto queued while SOAP is in flight is handled before classification.
The final two seconds of the last owned item remain natural STMu. The last
confirmed item position is retained when a renderer clears URI/RelTime at end.
Other definite stops release ownership and publish STOPPED before one CLI stop.
External pause/resume sends pause 1 / pause 0 once. Echoed LMS controls neither
repeat the speaker command nor create another CLI mutation. Only the dedicated
speaker-command function may mutate LMS; it validates command and reason, uses
a fresh bounded connection and never blindly retries an uncertain mutation.

Reference logic was studied in philippe44's LMS-uPnP renderer-state handling
and squeezelite SQ_PAUSE/SQ_STOP handling. No reference implementation code is
incorporated. App next/previous/seek mapping and general intent interpretation
remain parked.

LMS may echo an external resume as q+s when its delivered stream is exhausted.
For a fully produced current item with no newer producer, the relay pairs that
echo for three seconds and validates unchanged LMS index, source URL and time
(within 500 ms). Existing core cached-run support completes the HTTP/STAT
handshake while the same immutable item keeps playing: no Stop, enqueue or Play.
A q without a paired s is honoured after 250 ms; a changed LMS selection is
rendered normally. The gate sends no additional CLI commands and creates no
epochs, generations or rearm cycle. A confirmed foreign transport source takes
precedence over an error report, so error cleanup cannot Stop that source.

Intentional finite-item cancellation has a distinct exception type. Stale
worker jobs for cancelled items are discarded without poisoning the next
producer; genuine storage and length faults retain their existing handling.
A confirmed foreign source blocks transport commands and future appends during
the echo window as well as after release. Source switches, forced stops and
external resume are also recognised after an LMS-originated pause.

A producer flush does not revoke fully rendered immutable WAV assets already
accepted by the speaker. Their published length and bytes remain available;
only unfinished assets are cancelled. Relay end detection uses the announced
frame count, so a producer cancellation racing an observation cannot masquerade
as an external speaker action. A cancelled partial item that actually stops is
reported as a producer failure (STMn), never as an app-originated LMS stop.

### Playable source counts and short EOFs

Finite queue promises use the codec's native sample count and rate. For M4A,
media/edit timescales, ALAC frameLength and iTunSMPB original/priming/padding
select the playable sample interval. Padding already omitted by stts or a short
final packet is not removed twice; a final zero-sample padding packet is legal.
LMS's displayed sample-size label does not override the ALAC cookie.

Both ALAC and FLAC reconcile counts before resampling and mixing. An EOF
shortfall no greater than one codec packet **and** 100 ms is padded exactly,
logged once and continues without Stop/STMn. Larger discrepancies remain
source faults: cancel the incomplete asset, Stop/release once, report STMn,
and keep the storage worker healthy. Source overruns are truncated to the
published length and logged. Unknown lengths retain sealed publication;
malformed packets/CRC errors and empty sources remain failures. Stream-mode
rooms retain their existing pipeline and wire behavior.

See [precise length rules and verification](queue-hires-decimation.md#playable-lengths-and-exceptional-eof-repair).

## Relay source map (24 October cleanup)

The state table and its single serialized worker are unchanged. All modules
share the private `QueueRoom::Impl` declaration in `queue_room_internal.h`;
member order, mutexes, atomics and lock acquisition order are retained.

| File | Responsibility |
|---|---|
| `queue_room.cpp` | Construction, callbacks, lifetime, public facade and core-client entry point |
| `queue_relay.cpp` | Owned queue transport/state table, append/release, poll scheduling and the sole worker loop |
| `queue_path.cpp` | Source metadata/counts, relay-versus-stream selection and boundary handoff |
| `queue_speaker.cpp` | Speaker observations and external stop/pause/source-change detection |
| `queue_lms_commands.cpp` | The single reason-checked speaker-originated LMS mutation choke point |
| `queue_status.cpp` | Synchronous room status and existing relay-change log publication |

No threads, queues, locks or owner log formats were added or changed.
`tests/queue_refactor_compare.py --baseline c3baa3d` checks the mechanical
method/state extraction; it is a one-off comparison tool rather than freezing
future features to these method bodies. The next app-action feature can extend
the speaker detector and LMS command module while retaining the relay state table.

## Coordinator and fault policy (24 October 2026)

Group size never selects the per-track path: local file:// uses the relay;
finite non-live remote items also use relay; live/unknown-duration sources use stream. Queue output is already 24-bit 44.1/48 kHz,
so no group sample-rate rule is needed. At each new LMS start the worker reads
live GetZoneGroupState; every queue write retains uuid/name/ip identity fencing
and verifies that this room is still coordinator. Existing topology GENA and
polling update mid-play role changes; no new periodic SOAP traffic is added.
Adding/removing members while staying coordinator does not interrupt playback.
A role/source change reuses the external-stop detector. A member must not write
transport, including cleanup, since this can affect its new coordinator. Local ownership, lease and owned-item bookkeeping are released immediately;
no foreign source is stopped or switched. Both paths report one reason-checked
raw-MAC LMS stop through queue_lms_commands.h/.cpp.

Room faults are latched until a later new play checks topology. Source failures
are distinguished from storage/worker, SOAP, ownership and transport failures.
The host opts into core queueHostFailures so even a source receiving zero bytes
is classified before any STMn; other hosts retain the old default. Two consecutive
source failures within ten seconds stop the room after the second instead of
walking an album. A completed, fully produced audible item clears that streak when its successor starts.

For continuous streams only, existing STMs/STMt ticks schedule a read-only current-track
metadata query on the bridge control loop (at most the existing one-second
STAT cadence). This follows LMS processing its start notification without
assuming that another CLI connection has already seen that state. Sanitised/formatted title and duration
feed only the status display/log, not DIDL, HTTP or transport. The normal first
Play metadata and its cached renderer title keep their existing wire behavior.

## Queue and stream title policy

Queue musicTrack `dc:title` is the plain XML-1.0-sanitised LMS title, subject to the native Podcast title setting below.
Artist, album and artwork retain their separate fields; prefixing the artist
would duplicate what Sonos already displays. The queue path has no non-empty
fallback: an empty title stays empty, as in its existing title-only behavior.
The stream-only `Squeezebox` fallback is not borrowed for queue items.

`YENEY_TITLE_FORMAT` applies only to the stream path: its radio-type DIDL shows
only the title, so artist-title remains its default. Stream DIDL, sent-title
logging and continuous-stream display formatting are unchanged. Accepted queue
items log their title once; the current item's same sanitised title is published
as `playback.title` and used in the queue status display. JSON/log quoting
preserves valid XML whitespace and Unicode. All of this also works for direct
binary runs, using the existing state-directory/environment/stdout mechanisms.

### External takeover release

The last controller wins; LMS takes over again only on a new play.
A foreign AVTransportURI or group-member role releases local ownership immediately,
without Stop, queue removal or crossfade restoration. The external-stop path sends
one LMS stop and logs the source class with foreign identifiers redacted. It never
retries or automatically takes the speaker back. A later strm s is a fresh admission,
including the usual verified backup of a foreign queue.

Release clears local ownership and owned-item bookkeeping before best-effort SOAP
cleanup. A cleanup failure logs once and cannot retain the lease or fault the room.
A 7xx transport refusal means transport is no longer ours and ends the cleanup plan.
Crossfade restoration is skipped after takeover, including when the room joins
another coordinator. These rules also apply when running without systemd.

## Per-player volume choice

A future LMS plugin exposes `room.<name>.lms_volume=off|speaker` and
`room.<name>.volume_max=0..100` in the existing global configuration as
per-player settings. Runtime CLI/environment overrides, status and stdout logs
also support direct starts without systemd. Default off retains the original
traffic and bit-perfect PCM. Speaker mode reads exact LMS mixer volume/mute,
uses RenderingControl locally or GroupRenderingControl for a coordinator with
members, debounces writes and mirrors observed app changes through the sole
reason-checked LMS mutation choke point. No audio gain is applied.

The private real LMS 9.1.1 test renders Settings > Player > Audio for the actual
Model=yeney HELO. Native digitalVolumeControl is absent because HasDigitalOut
is not advertised (SqueezePlay default 0); the server's audio settings only
expose that preference for digital-output players. Thus YeneY configuration is
the sole mode source for the current model. Do not set the internal preference
or change HELO merely to expose it: that would change default wire behavior.

Group volume uses SnapshotGroupVolume before each settled SetGroupVolume to
preserve relative member levels, following [Sonos volume guidance](https://docs.sonos.com/docs/volume).
Snapshot and level/mute writes share the 200 ms rate limit. A snapshot never
counts as confirmation of a volume change.

## Finite remote queue evidence and implementation (27 October 2026)

Owner-run probes, S2 firmware 86.10-81100:

| Probe | Observed behavior | Implementation consequence |
|---|---|---|
| DIDL duration contradicts media header | DIDL duration ignored; WAV data size / FLAC total_samples wins | Derive WAV, HTTP length and DIDL from one frame count |
| Direct transport versus queue item | Duration/progress appears in the app only for queue items | Finite remote sources use Q:0 |
| Header three seconds shorter/longer | Short declaration truncates audio; long declaration advances at real EOF | Upper-bound promise plus continuations |
| 40 s tone, first item declares 30 s | Second item plays remaining 10 s seamlessly; embedded second header ignored | Immutable headers; overflow gets another queue item |
| Far seek and 120 s pause | Chunked fails; constant bitrate with Content-Length and standard 206 works | Progressive PCM WAV and persistent compressed spool |
| 33 s declaration, 30 s body | Empty 206 at real EOF advances immediately without error | Short EOF and at-end Range close without padding |
| Fetch pattern | GET probe (~32 KiB), HEAD, GET; next fetch 6–7 s before declared end | Resolve exact continuation by 10 s lead, otherwise publish generous bound |
| Publisher / scanner duration | Feed whole seconds may change after scanning; inserted adverts may add minutes | LMS duration is an estimate, never an exact truncation point |

Source classification uses LMS duration and `live_edge` (tag V): positive finite
duration with absent/negative live edge selects `remote-finite`; nonnegative live
edge or unknown duration preserves the existing continuous path. Group size is
irrelevant. The first remote promise is `ceil(duration*rate)+5*rate`. The status clock is
recorded but never subtracted from this conservative larger bound.
Local exact promises and their WAV bytes are unchanged. Overflow stays unpublished in the compressed spool until the producer end is resolved or the preceding segment has ten seconds left. It then publishes one exact remainder or a generous upper bound (produced overflow plus the original parent declaration, capped by room quota and WAV limits). Only exceeding that bound creates another continuation. Segments carry the parent's
metadata, are inserted in parent/segment order, and report one continuous LMS
track clock without an extra STMs. The relay logs insertion lead relative to the
10 s prefetch deadline, including negative margins for late production.

Estimated items permit short production without cancellation or LengthFault.
The declared header and Content-Length remain immutable. A Range inside the
announced entity but at/beyond actual EOF responds 206 with the requested offset,
declared last byte and declared total, announces the remaining declared length,
sends no body and closes. Future ranges wait on the existing bounded reader;
timeout closes and logs without fake bytes. Every segment remains spooled.

Quota decision: estimated playback raises the default room budget from 256 MiB
to 8 GiB, leaving explicit quota settings authoritative. The three-hour 44.1 kHz
incompressible 24-bit stereo measurement produced 476,280,000 frames,
2,857,680,000 PCM bytes and 2,868,842,882 compressed disk bytes in 27.6753 s.
Both the first and last frame remained readable. Current episode segments are
protected as one group. Old unprotected items can be evicted by the existing
policy; a reader of evicted history fails and closes. An undersized explicit
quota faults playback rather than producing fake audio. No systemd requirement.

The owner must run the new `remote-finite` device scenario to qualify real
speaker startup, 120 s pause/resume, midpoint REL_TIME seek and the audible tail.
Loopback tests cannot establish physical audibility.

### Native LMS fixture behavior

The generated feed advertises 60 s, 64 s, 30 s and a 60 s episode resumed from
20 s. Native FLAC scanning replaces publisher durations with its exact 60 s
count. The regression therefore also serves generated MP3 with a late bitrate
change: LMS estimates total duration from Content-Length and the initial
bitrate, retaining an overestimate near 64 s and an underestimate near 31 s for
about 60 s of actual decoded audio. This exercises real continuation production
through the private LMS, not only a replay peer. The native podcast `{from=20}`
resume marker is verified with an MP3 source, where LMS sends the remaining
approximately 40 s. Queue position after an app-equivalent seek to 30 s appears
in LMS as approximately 30 s through the existing STAT clock; no CLI seek is
added. Estimated DIDL retains fractional seconds derived from declared frames;
local DIDL retains its existing whole-second wire format.

A continuation uses the exact remaining frame count when the producer's end
resolves before the ten-second deadline. Existing published promises stay immutable;
unpublished overflow stays in the compressed spool until the end is resolved or ten seconds remain before the preceding segment ends. Exact completion publishes one exact remainder; the deadline publishes overflow produced so far plus the parent full duration, capped by room quota and WAV limits. Only exceeding that generous bound creates another segment.

A source successor arriving while a class handoff awaits renderer confirmation
is still a track boundary. The temporary STOPPED state is not idle: selection
uses the next LMS index. A short unknown-duration radio EOF regression exercises
this race without changing the stream wire path.

Cached resume from a continuation matches the parent LMS producer, retains the
same queue assets, and reanchors the existing cached-start handshake to that
parent. It does not turn the continuation into another LMS track.

## 28 October field failure: YENEY-PODCAST-02

Study on bridge 9c728e2 stopped a 33:19 episode after 63 seconds. The journal
contained TRANSITIONING, an immediate bridge STOPPED/STMu, then queue removal.
A second start was torn down by LMS strm q immediately after the app pause.
The field record alone does not establish the owner's original bookmark.

The old bound subtracted pre-start status time without proving its association
with the new producer. Native LMS 9.1.1 exposes full duration and absolute time
for a generated podcast continue entry; provider metadata remains an estimate.
The safe bound now keeps the positive duration without subtracting that clock.
Continuations have independent publication/lifecycle fields: a parent's flush
marker must not make new overflow records invisible. PODCAST-02 published them immediately; PODCAST-03 uses the exact/deadline rule above.
Creation, attempt and confirmation are separately observable.

TRANSITIONING is ignored as end evidence. An estimated episode can finish only
once production is complete and the played position reaches all produced audio
across its segments. Core also defers STMd until that point, with STMd preceding
STMu. A declared-boundary stop with audio still unplayed is a track fault.
Local exact promises, PCM, SOAP ordering and pause handling keep their existing
behavior. Remote pause-flush adoption verifies LMS mode=pause and source identity;
stop/selection changes retain normal release semantics. The cached resume gate
continues to validate index, source and clock before adopting an LMS restart.

Native Podcast scanUrl writes player-formatted current title back into TITLE.
Queue selection strips the exact artist/album formatting suffix for podcasts
and retains the first plain title by source identity, excluding native bookmarks.
The audio fake opts into declared WAV length, short real EOF, 6.5-second prefetch,
mid-item TRANSITIONING and pause/resume GET probe plus byte Range. Existing
control-only fixtures keep their explicit position controls.

The field pause mechanism is LMS's remote input-full policy. A paused decoder
previously filled the 256 KiB input ring; `_CheckPaused` then stopped the remote
stream and saved a resume position. Finite sources now have a bounded, unlinked
encoded disk FIFO under the room store. Its capacity is at most half the room
quota and fits the STAT 32-bit field; actual encoded high-water bytes and PCM
share quota accounting. A wraparound reader uses only an 8 KiB cache, and flush
closes the backing descriptor. Local and continuous input still use the original
RAM ring. Reception can finish while PCM output is paused, preserving the same
unfinished decoder and sample sequence through ordinary native p/u.

The native regression pauses before production completes for 120 real seconds.
Its progressive speaker closes the HTTP reader, then probes GET 0 and resumes
with Range at the exact paused frame. It compares the entire received PCM with
an independent decode of the actual MP3 Range served to LMS, including the final
second. Another controlled full-buffer STAT stimulus exercises native q+s and
verifies completed-spool reuse. The native status query temporarily reports
new seek offset plus old-run elapsed before cached STMs reanchors it; the gate
checks that equation explicitly. Finite pause also refreshes the paused position
and waits for a read-only LMS clock acknowledgement before CLI saves resumeTime.
No CLI seek, artificial silence, timer margin change or systemd dependency.

The field journal did not capture the actual declared WAV/HTTP length, so the
inferred 63-second declaration and presumed overflow remain hypotheses. New
promise, HTTP, continuation and end-decision lines make those values observable.

A reproduced transport cause can also explain a buffered stop: finite WAV writes
previously treated a 500 ms `EAGAIN` from a healthy full receive window as EOF.
The reader-stall regression now constrains that window, stops reading, then
compares every returned byte. Transient writes retry from the exact socket-byte
offset; peer close, cancellation and real errors still terminate. The existing
socket deadline is unchanged. This fixes backpressure semantics rather than
extending a timer. A natural remote end also retains successor admission credit
so the following LMS item uses its own metadata and an already-confirmed boundary.

Owner read-only metadata also confirmed that the cached `songinfo` title may
contain the native by/from presentation while artist/album tags are absent. A
bounded matching-feed menu lookup now supplies the canonical plain title on cold
starts. Menu parsing and lookup are tested against actual private LMS menus; no
owner gateway endpoint is used by automated tests. A final continuation shorter
than the observation interval can be seen first as STOPPED: its final owned URI
and completed production can finish the already-audible parent, without another
LMS start. Unrelated unconfirmed sources retain the PLAYING requirement.

### Podcast metadata settings

Native LMS Podcast sometimes supplies only a formatted title. The matching
read-only feed menu supplies the plain episode title and podcast name; its
exact suffix identifies the publisher without splitting names containing
` by ` or ` from `. Ambiguous fields stay empty with a diagnostic. The existing
two-second lookup budget and canonical source URL cache are retained.

Process environment takes precedence over `room.<name>.<KEY>` in
`/etc/yeney/config` (`YENEY_CONFIG` override), then the global `<KEY>`, then the
default. Each setting accepts `yes|no`; invalid values warn once and use its
default. Effective values and sources appear once at startup and in room status.
This works for direct starts without systemd. LMS's own artist/album win;
local tracks and the LMS display are unchanged. Continuations inherit metadata.

| Setting | Sonos app behavior | Default |
|---|---|---|
| `YENEY_PODCAST_TITLE_PLAIN` | `yes`: plain episode; `no`: LMS formatted title unchanged | `yes` |
| `YENEY_PODCAST_ARTIST_SHOW` | `yes`: podcast name as artist; `no`: send empty inferred artist elements | `yes` |
| `YENEY_PODCAST_ALBUM_PUBLISHER` | `yes`: publisher as album; `no`: send an empty inferred album element | `no` |

A future LMS plugin exposes these three metadata settings as per-player checkboxes.

At the ten-second deadline, an unfinished producer gets its upper-bound fallback
even if no overflow is available yet. If the real source ends inside the parent
bound, the unused fallback closes at real EOF without padding, another LMS start
or a jump in the episode clock. Further items require an exceeded fallback bound.

### Sonos queue DIDL evidence (30 October 2026)

On a Sonos Port with bridge f97fc73, the default podcast settings showed
"Geen content". GetPositionInfo returned the queue.wav file name as title:
the speaker had discarded the supplied DIDL. The owner's 30-second probe used
title "Media smullen van Trump versus Obama" and artist "Amerika in 15 minuten".

| Variant | Fields after dc:title | DIDL retained | Sonos app |
|---|---|---|---|
| a | dc:creator + upnp:artist; no upnp:album | no | nothing |
| b | dc:creator + upnp:artist + empty upnp:album | yes | episode + podcast name |
| c | empty dc:creator + empty upnp:artist + empty upnp:album | yes | episode only |
| d | dc:creator + upnp:artist + upnp:album BNR Nieuwsradio | yes | episode, podcast name and publisher (scrolling line) |

Queue DIDL always sends dc:title, dc:creator, upnp:artist and upnp:album in
that order. Disabled or unknown values are empty elements. With default settings
(plain title yes, artist yes, album no), the app shows episode and podcast name.
Artist no leaves the episode only; album yes adds the publisher when known.
Title plain no preserves LMS's formatted title. LMS's explicit artist/album
values still win over inferred values. Continuations inherit the same metadata.
