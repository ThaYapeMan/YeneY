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

Poll at 5 Hz during PLAYING/TRANSITIONING and 1 Hz otherwise. Subscribe to both
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
clock; the unchanged core's queueRelativeClock option is disabled in the host.
UPnP retains its observation-origin subtraction. A precise clock subtracts the
selected producer seek offset instead, preserving position before observation.
No core source or pin change is required. All STAT milliseconds already pass
unchanged through QueueMode::elapsed, player.cpp and the STAT wire fields.

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

## Loopback evidence and acceptance

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
