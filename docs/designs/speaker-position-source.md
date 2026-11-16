# Speaker position source

Queue playback can observe the read-only Sonos local API over a persistent TLS
WebSocket on port 1443. It uses the public application key, a self-signed device
certificate and the `v1.api.smartspeaker.audio` subprotocol. No account or audio
transport changes are involved. The exact owned UPnP CurrentTrackURI remains
the authority for audible queue identity; the local itemId triggers an identity
refresh but is not interpreted as an owned queue ordinal.

## Source selection

`--position-source=auto|websocket|upnp` overrides YENEY_POSITION_SOURCE, then
room.<name>.YENEY_POSITION_SOURCE in YENEY_CONFIG, then the global key, then
auto. Auto and websocket attempt the local API and fall back to the existing
UPnP observation path. Socket/command errors invalidate the source; reconnects
retry after 30 seconds. A missing usable sample for three seconds selects UPnP.
The ordered PositionSources registry accepts further implementations of the
SpeakerPosition interface. Source name and precision are properties of each
implementation. The room status publishes position_source and position_precision
and every source switch produces one journal/stdout line.

## Fit and consumers

Poll at 20 Hz during PLAYING/TRANSITIONING and 1 Hz otherwise. Subscribe to both
playback and metadata events, requesting a poll immediately on a state/item
event. Sample time is the send/receive midpoint. Discard RTT above 200 ms.
PositionFit retains a 2.5-second window and uses the median of offsets at rate
1.000. At the measured approximately minus ten ppm device drift, ignoring rate
change over that window contributes at most 0.025 ms. Median offsets prevent
an isolated slow observation from shifting an established fit. An initial
sample is usable immediately. State/item changes reset immediately; an error
above 150 ms requests an immediate confirming poll and two agreeing samples
restart the fit. Pause freezes the clock.

Speaker seeks use millisecond differences against real-time advancement.
Rewinds retain the index/token selection authority, exactly one integer time
command, and no renderer Seek echo. The ticket records that integer target.
The fitted source is queried when the core asks for elapsed, rather than only
when the room worker polls. The bridge's QueuePathSink supplies the relative
clock; the core's queueRelativeClock option is disabled in the host.
UPnP retains its observation-origin subtraction. A precise clock subtracts the
selected producer seek offset instead, preserving position before observation.
Player::requestStatus asks the core for a current STMt on its next control-loop
turn, coalesced to at most one extra report per 50 ms. It does not move the
periodic heartbeat. The bridge asks on confirmed discontinuity, audible item,
playback state or source change, and once a renderer-seek/rewind position is
known. STAT milliseconds pass through QueueMode::elapsed and the existing wire
fields. A pre-seek fit cannot announce a newly selected producer. A successful
renderer seek invalidates the cached UPnP identity before it can authorize
navigation against the old row.

Grouping re-reads local/info and follows its advertised secure websocketUrl,
including a coordinator at another numeric address and port. The coordinator's
own local/info verifies household/group identity. The deterministic test uses
two independent clocks at 127.0.0.1 and 127.0.0.2, with no source faults and
less than one second to group or ungroup.

The former path parses whole-second RelTime, holds it until the next poll and
subtracts the first observation as its clock origin. Truncation, sample age and
observation-origin subtraction account for lag; successive poll phases and the
LMS interpolation updates create level changes. This mechanism explains the
kind of error in the owner's evidence; the exact contributions on the devices
cannot be separated from those two captures alone.

## Device evidence, 14 November 2026

On the owner's Port the two 120-second, 4 Hz comparisons of positionMillis
minus LMS time had medians +991 and +1047 ms. About 78 percent fell between
900 and 1100 ms, 13–16 percent above 1100 ms, and 6.5–8 percent below 500 ms
(minimum 18 ms). There were 61 and 14 changes above 150 ms; creep was +0.36
and minus 0.68 ms/s. A separate core player against the same clock had 1.6 ms
standard deviation, no jumps and approximately 0.01 ms/s creep.

The earlier Port probe measured 999.98 ms/s, residual median 1.7 ms and p95
9.6 ms at 5 Hz; RTT median approximately 10 ms with occasional 50 ms spikes.
Firmware was 86.10-81100, API 1.54.2. This task does not contact those devices.

## TIMING-03 historical loopback evidence

The TLS fake models the handshake, local info, subscriptions, status messages,
continuous position, symmetric RTT and occasional 50 ms spikes. Its group ID
can change, and socket closure exercises the 30-second recovery. The real
LMS comparison uses disposable LMS 9.1.1 and generated 180-second tracks.

The first end-to-end implementation was steadily 53.794 ms late. This exposed
the observation-origin subtraction in the core's relative-clock option. With
that calculation moved into the bridge, a 480-sample 120-second run measured
LMS minus true position median -0.834 ms, 100 percent within ±30 ms, zero jumps
above 100 ms, and creep -0.001221 ms/s. App seek and rewind detection were
202.134 and 202.398 ms. Subsequent rewind testing exposed the old-owner/new-
audible-occurrence distinction and the integer rewind ticket coordinate;
the later final-binary run confirmed their steady post-rewind clock. Its
120-second steady median was -1.444 ms, 100 percent within ±30 ms, zero jumps,
and +0.000182 ms/s creep. Source detection was 201.837 ms for app seek and
201.916 ms for rewind. Post-seek, post-rewind and boundary medians were -0.795,
-1.249 and +0.466 ms, all samples within ±30 ms and no jumps. UPnP position
polling fell to 58.047 polls/minute.

**Not accepted:** LMS alignment after those events took 411.048 ms and
374.903 ms, exceeding the 300 ms requirement. Reading a current fit at STAT
construction fixes steady playback but does not itself request an immediate
STAT between the unchanged core's one-second periodic reports. Rewind also
passes through the existing CLI/producer restart. The eight-second post-seek creep estimate was -0.160992 ms/s, also outside
the stated 0.1 ms/s target. No extra position mutation,
core modification or threshold relaxation was used to conceal those misses.

The forced-UPnP comparison measured steady median -474.442 ms, zero percent
within ±30 ms, 72 jumps and +0.135204 ms/s creep. Its seek/rewind detection
samples were 201.574 and 101.020 ms, so this stimulus does not establish an
approximately one-second detection delay. After rewind the median was
-1008.617 ms. These measured baselines must not be replaced by expected figures.

The full report contains final acceptance results and any remaining failures.
A steady clock result alone is not complete acceptance.

## Owner diagnostic and later fallback

scripts/sonos-timing-probe.py --source both requires --speaker, --lms-host,
--lms-port and --player-mac. The default run is 120 seconds, with paired clocks
at 4 Hz and GetPositionInfo at 10 Hz. It prints the owner's bands, jumps,
creep and TSV path, plus RelTime truncation/rounding evidence and second-change
phases for steady playback, boundaries and seeks. Perform a boundary and a
seek during the run to populate those categories. Missing events are printed
as n=0/nan, never invented as passing measurements. Raw timestamps preserve
both sides of each sample bracket.

No precise UPnP edge fit is implemented. That later source can register through
the same interface once the owner's second-change measurements validate it.
The SHM v1 prefix and timing record layouts remain unchanged.

## TIMING-03b measured loopback figures

The final timing validation after the core changes uses 20 Hz WebSocket polling
while playing and a 20 ms room observation interval. The former 5 Hz polling could still produce a
345.473 ms rewind despite immediate status requests. Keep the existing 1 Hz
paused cadence, one-second UPnP identity refresh and UPnP fallback schedule.
Midpoint LMS queries run at 4 Hz; RTT spikes are enabled. Steady playback uses
120 seconds and every event window uses 60 seconds. Creep is computed only
on windows at least 60 seconds long.

| Window | Samples | Median LMS minus true ms | Within ±30 ms | Jumps >100 ms | Creep ms/s |
|---|---:|---:|---:|---:|---:|
| steady | 480 | -0.812667 | 100.000% | 0 | 0.003376 |
| after-app-seek | 240 | -1.091641 | 100.000% | 0 | 0.000464 |
| after-rewind | 240 | -1.171680 | 100.000% | 0 | -0.003549 |
| after-lms-in-item-seek | 240 | -0.858553 | 100.000% | 0 | -0.008924 |
| boundary | 240 | 0.098543 | 100.000% | 0 | 0.004323 |
| after-row-switch | 240 | -1.181051 | 100.000% | 0 | 0.007424 |

| Event | First aligned LMS reading ms |
|---|---:|
| app-seek | 101.308 |
| rewind | 266.394 |
| lms-in-item-seek | 107.455 |
| boundary | 117.776 |
| row-switch | 152.188 |

The row-switch check exposed a new row being announced against the old row's
fit. A fitted sample now retains the midpoint of its last accepted observation
separately from its projected position time. After a renderer seek, the room
requires an accepted observation newer than the completed write before
announcing the new producer. It does not substitute the requested seek position
for an observed position. Renderer seek completion also invalidates the old
cached UPnP identity; neither stale observation can authorize navigation.

The owner evidence of 14 November above is unchanged: the two Port captures
had roughly one second of LMS lag. These container-local measurements establish
timing behavior after the core changes, not a new device comparison or
deployment. The core suite passes. Full-gate acceptance was not achieved
within the three-attempt limit; subsequent validation is reported separately.


## Local API wire compatibility (16 November)

The Port on 98.0-82170 returns chunked local/info with `Content-type`,
`Transfer-encoding` and `Connection: close`. Requiring Content-Length prevented
selection before any WebSocket connection. The bounded production reader now
supports all three body delimiters and case-insensitive HTTP header/token
reading, including the upgrade. The fake defaults to that captured shape.

The binary's `--position-source-check --ip <speaker> --seconds 30` shares the
production session and parser, testing 5 Hz for half the duration and 20 Hz for
the other half. Only subscriptions and getPlaybackStatus are sent. A fit sample
accepted within three seconds is required for a successful WebSocket selection.
Playing residuals are measured against a fitted straight line, so keep playback
steady during this check. Real hardware performance must be measured by the
owner; loopback success does not establish speaker capacity at 20 Hz.

Status publishes the source reason even when the source does not change. Each
new failed connection attempt is eligible for a journal reminder, limited to one
per ten minutes. Polling cadence, fit, navigation and core pin remain unchanged.


### Captured states and change events

The local API state mapping lives in `speaker_position.h`. PLAYING advances
the clock; PAUSED and IDLE hold it; BUFFERING holds it as a transition while
keeping the fast polling cadence. Unknown values hold position, appear in room
status, and are journaled once per room and raw value. The diagnostic fails
when it observes an unknown value.

A changed playbackStatus event contributes an arrival-time sample with no RTT.
It immediately resets the fit, requests status, and schedules a confirming
getPlaybackStatus. A valid command answer overrides the event, including a
contradiction; a discarded slow answer cannot remove the event clock. Repeated
state/item/expected-position information does not reset the fit or request
status. Every event retains previousItemId and previousPositionMillis for
logging. Same-item seeks log their from/to positions; a completed exact promise
logs the previous item's final position and promised duration. These logs do
not change navigation or length enforcement.

Local itemId values are queue-row strings, not URI identities. A row change
requests an immediate identity poll. UPnP's exact owned URI remains authoritative;
a numeric row disagreement is journaled once. Nonnumeric IDs are accepted.
Metadata quality and playbackError events are diagnostics only: quality is
journaled per item and appears in status, and each error occurrence is journaled
with its last value retained in status. A globalError command response rejects
the command with its error code.

The public documentation sources and capture precedence are listed in the
README's “Sources for the Sonos local API” developer section. The requested
GlobalError URL returned HTTP 404 during TIMING-03d; the supplied owner summary
provides its command-error requirements. The public previousPositionMillis
table says string but its example and the local capture use a number. The
client accepts either representation. Captured quality, locationId, local
response headers and extra action fields are retained by the fixture even
where the public cloud reference does not list them.
