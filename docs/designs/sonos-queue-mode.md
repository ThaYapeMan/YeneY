# LMS-driven Sonos queue relay

The queue-mode base is a thin relay. LMS is the sole playback master. Stream
mode remains the default and its transport, URLs and wire fixtures are unchanged.
Explicit `YENEY_PLAYBACK_MODE=queue` selects the relay for a standalone room.
Grouped rooms use stream with `group-rate-undecided`; `auto` still uses stream.
There is no automatic fallback inside this queue release.

The previous advanced implementation is preserved by immutable annotated
`queue-full-v1` tags in both repositories (bridge `98727db`, core `13db8b1`).
[Parked features and rebuild checklist](queue-parked-features.md) gives the
files, commits, tests, device evidence and future rebuild order. It replaces
all earlier claims of advanced controller/device qualification.

## State table

One serialized worker owns SOAP and an ordered list of immutable WAV assets,
each identified by the core's monotonically allocated LMS stream occurrence.
The only player states are **STOPPED**, **PLAYING**, **PAUSED**. Pending render
records are data, not a separate player state. No playback epochs, generations,
rearms, intent guessing, readiness continuation or LMS CLI mutations exist in
the relay. A fixed legacy route namespace keeps the existing WAV URL shape;
it is not a playback generation and never causes a capability announcement.

| Input/evidence | Renderer work | SlimProto result / next state |
|---|---|---|
| Genuine LMS connection | None | Exactly one HELO on that connection |
| LMS `strm q` | Stop, remove exact owned URIs, restore CrossfadeMode | Normal core producer reset, STMf, STOPPED |
| LMS `strm s` following q or idle | Render the selected source/offset; publish once exact length is known; acquire queue; AddURIToQueue; select `x-rincon-queue:<UDN>#0`; Seek TRACK_NR 1; Play once | Normal stream connection acknowledgements; remain STOPPED until speaker confirms playback |
| LMS successor `strm s` after our STMd | Render and append with AddURIToQueue; never issue another Play for the successor | Await its confirmed audible start |
| GetPositionInfo identifies a prepared owned item and GetTransportInfo says PLAYING | Feed confirmed item/frame position to the core and synchronously publish status | STMs once for that stream, PLAYING |
| Current item fully produced | None | STMd in a later core round than STMs; local successors may be requested early |
| Remote item fully produced, audible remaining < configured delay | None | STMd; next service connection is not left idle for the full item |
| LMS `strm p` / `strm u` | Pause / Play (resume only when paused) | Speaker-confirmed STMp/STMr, PAUSED/PLAYING |
| Speaker STOPPED on final owned item | Feed confirmed final state to core | STMu once, STOPPED |
| LMS seek (q+s with LMS source offset) | Same as a fresh play; no independent CLI/REL_TIME interpretation | New rendered stream, then confirmed STMs |
| SOAP or asset failure | Log fault; Stop; remove exact owned items; restore crossfade | One renderer-rejection STMn, STOPPED; no stream fallback/retry loop |
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

`YENEY_QUEUE_NEXT_DELAY_S` defaults to 20 (valid 1–120). A remote flag or non-file
source URL selects the delayed successor gate. `YENEY_QUEUE_DISK_MIB` remains a
per-room disk quota (default 256). Unknown exact length waits for sealing; the
relay never guesses a WAV length from LMS duration or an HTTP Range offset.

## Status and extension points

The suite reads the existing per-room `stream-pacing-<room-hex>.json` file's
`playback` object. Queue fields: requested/active=`queue`, reason=`queue-opt-in`,
state, occurrence, owned, owned_item_uris, current_owned_item, last_fault.
Each observed state/ownership/item change publishes an atomic complete file on
the same worker call, without waiting for the periodic stream metrics tick.
Speaker polling is every 100 ms; publication cannot precede an observation.
Stream diagnostic representation stays unchanged.

Future components attach to these boundaries, without changing the relay state
table: (2) a separate failure/release observer and stream sink selector for
fallback; (3) a speaker-evidence observer with explicit own-command matching for
app actions; (4) a mirror supplying append/remove plans for future items; (5) a
room eligibility/group policy outside the relay. None may fabricate an LMS start
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

Queue HELO now advertises the stream sink's 48 kHz maximum (44.1 kHz
with `YENEY_AUDIO=16/44`); finite queue WAV output remains 24-bit.
LMS 9.1.1's CapabilitiesHelper selects 44.1 kHz for 88.2/176.4 kHz
sources under a 48 kHz cap, and 48 kHz for 96/192 kHz sources. Its
standard FLAC transcoding rule uses SoX's default high-quality rate
conversion, preserving 24-bit input precision. No custom transcoding
rule or cross-family conversion is needed. The existing stream cap and
wire capabilities are unchanged, so stream mode uses that same LMS rule.
Legacy 44.1 kHz mode necessarily limits the 48 kHz family to 44.1 kHz.
An incoming rate above the advertised cap fails with STMn before enqueue.

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
