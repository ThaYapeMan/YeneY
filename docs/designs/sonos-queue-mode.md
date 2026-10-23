# LMS-driven Sonos queue relay

The queue-mode base is a thin relay. LMS is the sole playback master. Stream
mode remains the default and its transport, URLs and wire fixtures are unchanged.
Explicit `YENEY_PLAYBACK_MODE=queue` selects source-aware playback for a standalone room: local files use the finite relay and remote/unknown sources use the existing continuous stream path.
Grouped rooms use stream with `group-rate-undecided`; `auto` still uses stream.
There is no automatic fallback inside this queue release.

The previous advanced implementation is preserved by immutable annotated
`queue-full-v1` tags in both repositories (bridge `98727db`, core `13db8b1`).
[Parked features and rebuild checklist](queue-parked-features.md) gives the
files, commits, tests, device evidence and future rebuild order. It replaces
all earlier claims of advanced controller/device qualification.

## Per-track source/path selection

Classification runs on the room worker at each LMS `strm s`, before opening
its audio connection. A read-only CLI snapshot supplies the track admitted by
LMS. `remote:1`, a negative id, any URL outside `file://`, or missing/ambiguous
metadata selects stream. Only an explicitly local file selects the relay.
No selection, readiness or class boundary causes an LMS CLI mutation or HELO.

| Play/source | Active path | Boundary behaviour |
|---|---|---|
| New local file after q/idle | relay / local-source | Existing finite WAV takeover and startup |
| New remote or unknown source | stream / remote-source | Existing stream metadata, artwork, PCM feeder, HTTP and pacing; no queue acquisition |
| Next track has the same class | Current path | Existing append/continuous behaviour |
| Local → remote | Keep relay until confirmed final-item end | Stop/release owned entries and restore crossfade; fresh continuous start |
| Remote → local | Keep stream until submitted audio is audibly drained | Close the continuous session and Stop; fresh relay takeover/start |
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
rearms, intent guessing or readiness continuation exist in the relay. The only
CLI mutations report unambiguous speaker-originated stop/pause/resume. A fixed legacy route namespace keeps the existing WAV URL shape;
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
| SOAP or asset failure | Log fault; Stop; remove exact owned items; restore crossfade | One renderer-rejection STMn, STOPPED; no stream fallback/retry loop |
| External stop/source change, outside the 3 s own-command window, before the final 2 s | Release exact owned items without Stop or URI switch | One raw-MAC LMS stop, STOPPED |
| External pause/resume, outside the own-command window | Report confirmed state; suppress the resulting LMS echo | One LMS pause 1 / pause 0, PAUSED/PLAYING |
| Service shutdown | Stop and release exact owned entries; restore crossfade | Process exits |

These messages let LMS's StreamingController own song advancement: STMs reports
TrackStarted, STMd reports decoder completion / NextIfMore, STMu reports output
underrun/end, and STMn reports renderer acceptance failure. YeneY never responds
to LMS's own starts by jumping the LMS playlist. Read-only status queries supply
metadata; requests use the raw player MAC and replies are CLI-decoded once.

The optional core Sink generation API remains source-compatible, but its old
queue generation state/reset is removed. Default stream/WAV/SHM sinks do not
consume renderer-failure notifications. Core-rendered transitions, exact sample
counts, FLAC seek origins, finite container decoding and speaker clocks remain.

## Queue ownership and finite transport

On first acquisition foreign Q:0 entries are saved in an identity-verified
`Before YeneY <timestamp>` playlist, then removed with UpdateID fencing.
An empty or exclusively owned queue needs no backup. CrossfadeMode is read,
forced off and verified; release restores it. Removal refreshes Browse and
removes only exact owned URIs from the back, preserving foreign insertions.
Every append uses Browse-before/after fencing and requires NumTracksAdded=1.

Production uses only **AddURIToQueue** with InstanceID=0,
DesiredFirstTrackNumberEnqueued=0 and EnqueueAsNext=0. The retained unit-tested
AddMultipleURIsToQueue helper is **not device-verified**: Study returned 402 on
20 October 2026. Its rejection is unexplained and may have been the invalid
U+0016 album XML identified the next morning. It is not a production path.

The finite item store, progressive exact-length/sealed WAV publication, HTTP
Range/reconnect support, metadata DIDL, XML 1.0 sanitiser and shared artwork
resolver are unchanged. Metadata repair is logged by field/count, once per
sent item; the sanitised formatted title is sent consistently.
Sonos crossfade is off; LMS transitions are still rendered by the core. Queue
crossfade and mixed-rate owner qualification are parked, not declared passed.

`YENEY_QUEUE_NEXT_DELAY_S` defaults to 20 (valid 1–120). The core retains
its finite-remote successor gate for other queue hosts; production remote
sources now use the continuous path instead. `YENEY_QUEUE_DISK_MIB` remains a
per-room disk quota (default 256). Unknown exact length waits for sealing; the
relay never guesses a WAV length from LMS duration or an HTTP Range offset.

## Status and extension points

The suite reads the existing per-room `stream-pacing-<room-hex>.json` file's
`playback` object. Queue fields: requested/active=`queue`, path=`relay`/`stream`, reason=`local-source`/`remote-source`,
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
now implemented, but automatic recovery from failures and group/auto policy
remain separate future work. None may fabricate an LMS start
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
before DSCO. Unknown source counts retain sealed publication. Remote sources
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
seconds after Play faults once, stops/releases owned entries and sends STMn.
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
