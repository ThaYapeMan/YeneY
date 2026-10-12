# Sonos queue capability probe

This is an owner-run experiment for **queue variant A**: one Sonos queue item per
LMS track, with every audio URL served by YeneY and audio still produced through
yeney-core. It is not the resampling “Option A” in
[rate-change-gapless.md](rate-change-gapless.md), and does not implement either
redesign. No production transport, HTTP, encoder or core behavior changes.

The present continuous stream loses its buffered tail when a sample-rate change
switches the URI. The listening evidence in that design document suggests about
1.8 seconds discarded and 2.3 seconds restarting. Queue prefetch might remove
both, but must be tested on the actual model/firmware. The probe records those
identifiers and asks six separate questions before a bridge implementation is
justified. It also supplies native per-item DIDL titles and generated artwork to
check the app presentation.

## Before running

Use a comfortable listening volume. Stop playback for **Study (Sonos)** in LMS,
then stop **yeney@Study**. Keep other controllers from starting Study during the
experiment. In the Sonos app, disable shuffle, repeat and crossfade for the baseline trials.
Start with Study ungrouped. The probe refuses LMS `play` **and**
`pause`, an active room service, a playing/transitioning speaker, ambiguous room
selection or an already grouped Study. LMS mode is checked through its read-only
CLI. If LMS has forgotten a disconnected player, a confirmed `inactive` or
`failed` room service establishes idle and the report records
`stopped-service-player-absent`. An unknown service with an absent player,
duplicate player names, CLI failure or a listed playing/paused player still
causes refusal. It never sends LMS playback commands
or starts/stops services itself. A stopped player/service is only a start-time
check: an external controller can still interfere later.

A nonempty original queue requires `--replace-queue`. Save any wanted queue in
the Sonos app first: **this flag authorizes deleting the old queue**. The probe
records its length and transport URI, not its contents. Without the flag it
aborts before queue mutation. An empty queue does not need the flag.

Run on LXC 113 as root or an ordinary user; Python 3 and its standard library are
the only probe dependencies. No compilation, pip package, ffmpeg, flac executable
or privileged port is needed. The unit tests additionally use the project's
existing libFLAC to independently validate generated audio.

## Owner commands

These commands are for the owner; an agent must not SSH to or deploy on LXC 113.
In the LXC console:

```bash
cd /opt/yeney
git pull --ff-only
# In LMS: select Study (Sonos), press Stop (not Pause).
sudo systemctl stop yeney@Study
python3 scripts/sonos-queue-probe --replace-queue
# After the probe, including an interrupted or failed run:
sudo systemctl start yeney@Study
# In LMS: select Study (Sonos), press Play to restore normal YeneY playback.
```

Omit `sudo` when already root. Do not run `--replace-queue` unless losing the
original queue is acceptable. This diagnostic needs no `make install` and does
not enable timing overrides. If the checkout is owned by root, run the git
command with the existing repository owner's privileges.

By default LMS comes from `/etc/yeney/config` (`LMS_SERVER`), then LAN discovery;
Study is found by SSDP and ZoneGroupTopology. To avoid multicast discovery or to
choose the reachable LAN interface explicitly:

```bash
python3 scripts/sonos-queue-probe --lms LMS_HOST --ip STUDY_IP \
  --lan-ip 192.168.178.31 --port 8099 --replace-queue
```

Replace `LMS_HOST` and `STUDY_IP` with actual addresses. `--ip` is a seed speaker;
ZoneGroupTopology still selects the room named Study. `--room NAME` is available
for other rooms; stop that room's LMS player and service instead. Port 8099 must
be reachable from the speaker for audio, artwork and event callbacks. Use a
trusted LAN: this temporary HTTP server deliberately has no authentication or
TLS. The server binds to the route-selected LAN address, not all interfaces.

Expect roughly 15 minutes, plus time typing notes and any device timeouts. Each
trial explains what to listen/watch for and asks **whether it was clean,
complete and correct**: `y` means success, `n` means a gap/click/error or incorrect
behavior. Add a note such as `n silence at second boundary, about 2 seconds`.
For Q5 the tool opens a ten-second window for one app Next tap and another for
one app Previous tap; press Enter when ready, then tap once. Preserve app error
text in your note. Ctrl-C, SIGTERM or EOF attempts cleanup and writes a report.
Do not leave the probe unattended at its prompts.

Optional grouped experiment (about three extra minutes): stop LMS playback and
the YeneY service for the second room too, initially leave both rooms standalone,
and add `--group-with 'SECOND ROOM'`. Only after Q1–Q6, the tool asks you to group
that room **under Study in the Sonos app**. It verifies exactly those two rooms
and Study as coordinator, then repeats same-rate and both mixed-rate boundaries.
It does not issue grouping actions. Restore the original grouping in the app
when finished, then restart both stopped services and resume the wanted LMS
players. Q7 is absent unless requested.

## Output and copying

Every run prints a unique directory:
`/tmp/sonos-queue-probe-<local-date>-<time>-<nonce>/`. Four files survive normal
completion, refusal and ordinary interruption:

* `report.md`: initial model/firmware, transport URI, true queue length and
  topology; one verdict per question; listening notes and detailed measurements;
  completion/cleanup status. A GO is a capability observation, not bridge approval.
* `requests.tsv`: monotonic request begin, response headers, body progress and
  response/connection close; method, path, Range, status, offsets and bytes.
  GET, HEAD, artwork and event NOTIFY requests are logged. Header-row `bytes` is
  Content-Length; progress/close `bytes` is cumulative payload written for that
  response. HEAD and NOTIFY response bodies are zero length. A successful write
  means accepted by the local socket, not acknowledged or played by Sonos.
  `close` means our response ended or a socket error/shutdown occurred; it is
  not a measurement of the remote TCP FIN timestamp.
* `positions.tsv`: GetPositionInfo send/receive monotonic timestamps, Track,
  TrackURI, floored RelTime, duration, last transport state/status and errors.
* `events.log`: newline-delimited JSON with monotonic timestamps: SOAP commands,
  arguments/results/faults, raw AVTransport LastChange notifications and parsed
  instance-0 values, subscription renewal, app windows, observations and cleanup.

All files use the same host monotonic clock. GetPositionInfo is scheduled at
4 Hz and GetTransportInfo at 1 Hz. Slow/timed-out SOAP calls reduce the actual
rate; there is no burst of catch-up polls. The report exposes sample counts and
the TSV exposes every interval. An absent event callback is a condition even if
polling works; check LAN callback reachability before drawing a design conclusion.

The tool prints these concrete commands with its actual stamp. On the Proxmox
host (not inside the container), substitute the printed directory:

```bash
RUN='/tmp/sonos-queue-probe-<stamp>'
pct pull 113 "$RUN/report.md" "/tmp/${RUN##*/}-report.md"
pct pull 113 "$RUN/requests.tsv" "/tmp/${RUN##*/}-requests.tsv"
pct pull 113 "$RUN/positions.tsv" "/tmp/${RUN##*/}-positions.tsv"
pct pull 113 "$RUN/events.log" "/tmp/${RUN##*/}-events.log"
```

Replace `<stamp>` before executing; angle brackets are explanatory placeholders,
not shell syntax. Every destination is under `/tmp`. The probe never invokes pct.

## Audio and HTTP method

Items contain a quiet 440 Hz tone and a soft, smoothly enveloped 880 Hz beep
once a second, with identical stereo channels. Integer frequencies, whole-second
item lengths and a repeated one-second waveform preserve mathematical phase
across rate changes and boundaries. Listen for missing/late beeps, silence or
clicks rather than spoken announcements. Quantization differs between 16 and
24 bit; there is no promise that an acoustic capture will be bit-identical.
Titles identify the question, item, rate and bit depth, and every item has its
own URL and PNG artwork.

The tool builds finite WAV PCM (classic 44-byte header) and valid FLAC with
STREAMINFO duration/MD5 and independent verbatim stereo subframes. FLAC framing
and checksums follow [RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html).
There is no FLAC seek table. This intentionally trades compression for a pure
standard-library implementation; CPU/bandwidth and decoder seek behavior are
not representative of every compressed FLAC encoder. All formats have exact
Content-Length known before queueing, Accept-Ranges and single byte-range 206
responses (including open-ended/suffix ranges); invalid ranges get 416. HEAD
serves no body. Responses close their connections explicitly. Body writes allow
30 seconds of backpressure, so the intentional ten-second pause does not itself
force a disconnect; cleanup can therefore take up to about 30 seconds.

The native finite-file DIDL uses `object.item.audioItem.musicTrack` and
`http-get:*:audio/flac:*` or `http-get:*:audio/wav:*`, with size, duration, rate,
bit depth, title and album-art URI. The probe reads the device's advertised UPnP
service URLs. Playback mutations use only AVTransport queue/transport actions;
read-only Queue Browse supplies the actual queue length, GetMediaInfo the
original URI, and ZoneGroupTopology room/coordinator information. No cloud API,
ContentDirectory mutation, volume setting, LMS control or production bridge
HTTP endpoint is involved. A missing/incompatible advertised Queue service
fails the snapshot instead of assuming its queue is empty.

Q1, Q2, Q4, Q6 and Q7 use **24-bit FLAC**, the relevant format for YeneY's encoder.
Q3 isolates all four formats. Q5 tests byte-exact WAV seeks and additionally FLAC
seek/pause/resume, so a working WAV result cannot conceal a FLAC control failure.

For Q4 the first body GET starts a producer-availability clock shared by all
reconnects/ranges of that item. Audio becomes available at 1.0x or 1.2x; there is
no intentional startup reservoir. Seeking ahead waits until that byte's audio
has been produced. FLAC is released in small writes with a complete-frame time
mapping (up to one 4096-sample block of granularity), not by pretending encoded
bytes have uniform duration. Exact length is advertised even while delivery is
paced. This tests finite-length paced HTTP, not live LMS production itself.

## Questions and design consequences

| Question / trial | Evidence and interpretation | Queue variant A decision |
|---|---|---|
| Q1: three 30 s 44.1 kHz FLAC items | First next-item GET relative to estimated old-item end; both boundary observations; titles/art. Positive lead means prefetch. A header-only/probing Range is still a request, not proof of useful audio fetched. | GO if both joins are audibly clean. If only presentation succeeds, fall back to per-track title mode until joins work. |
| Q2: 44.1 → 48 → 44.1 kHz, 30 s each | Same lead bounds and listening in both directions. Compare with Q1; never infer gaplessness solely from early GET. | GO if both directions are clean. If Q1 works but Q2 fails, GO WITH CONDITIONS for a fixed session output rate using permissive resampling; otherwise per-track title mode. |
| Q3: FLAC 16/24, WAV 16/24, 30 s each | Positive PLAYING position evidence, requested URI, transport/error events and owner's completion/error notes for each exact-length format. | GO only for verified formats. Restrict output bit depth/container if necessary. WAV success alone requires a separate bandwidth/bridge change; FLAC failure is material. |
| Q4: 60 s FLAC at 1.0x then 1.2x | Owner's full-tail observation, visited item, possible ≥3 s PLAYING flat, errors, per-write audio horizon minus estimated playback time. | GO if real-time delivery sustains playback. Only 1.2x working is conditional on an achievable startup reservoir/producer lead; it does not prove LMS can produce 1.2x. If neither works, reconsider finite queued live production and use title mode or another delivery design. |
| Q5: controls | WAV seek to 12 s: requested Range offset maps to `(offset−44)/(rate × channels × bytes/sample)`; frame alignment and target difference logged. Ten-second pause/resume: requests vs retained connection/progress; transport/app errors. UPnP Next/Previous: command times, first subsequent audio GET delays and position changes. App Next/Previous: separate windows and notes. FLAC seek/resume separately. | GO if controls succeed for the chosen format. Existing buffered seek can legitimately cause no HTTP Range. Read-back before 12 s can be decoder priming, not wrong audible seek. Previous may restart the current item on its first tap; record that behavior rather than assuming a URI change. Missing controls mean app-control promises need limiting or title-mode fallback. |
| Q6: three mixed-rate items at 4 Hz | Track number against queue ordinal; TrackURI transitions and first new RelTime; within-item decreases, large jumps and flats. | GO if positions identify the item and restart consistently. Stable offsets/coarse events mean GO WITH CONDITIONS with a mapped per-item timing model; inconsistent identities/counters require further evidence before LMS synchronization. |
| Q7, optional: grouped same/mixed rates | Q1/Q2 repeated with verified Study coordinator and exactly two members; notes from both rooms. | GO for the tested group only; failures restrict initial support to standalone rooms or require rate conversion/group investigation. |

The prefetch interval uses the latest PLAYING position in the old item’s first
playback visit, ending at its first observed departure, and its
reported duration: `[send + duration − RelTime − 1 − GET,
receive + duration − RelTime − GET]`. Its midpoint is an estimate, with at least
the one-second RelTime quantization uncertainty and SOAP transit bounds. This
excludes Sonos resetting CurrentTrackURI to item 1 at queue EOF while
the cached state still says PLAYING; using that later zero inflated the first
boundary by roughly 90 seconds in the 12 October captures. It
assumes uninterrupted playback; Q5 intentionally omits this boundary calculation.
The range list includes initial/header requests as well as seeks; use the control
command timestamps to identify which requests belong to a seek. A positive
interval wholly above zero supports useful *request* lead. Negative
or overlapping-zero intervals deserve investigation. Duration metadata and
speaker rendering latency add uncertainty; none of this measures acoustic gap.

`max_socket_write_ahead_s` compares the served audio horizon with a nearby
PLAYING RelTime sample, extrapolated at unity with a half-second quantization
correction. WAV offsets are exact PCM time; FLAC horizons count complete frames.
This is an approximate sender-side read-ahead indicator, not actual speaker
buffer occupancy, TCP acknowledgment or decoding time. Control trials include
intentional discontinuities, so their read-ahead/jump data is diagnostic only.
For uninterrupted trials, within-item backwards reports and jumps greater than
wall time +1.25 s are recorded; repeated floored seconds are normal, while a
PLAYING flat lasting at least 3 s is flagged. Per-item reset is not counted as a
backwards step. Device error statuses and ERROR notifications are retained even
when the owner heard no problem.

Automatic verdicts are deliberately limited: missing evidence or possible stalls
is conditional; listening failure, SOAP failure or device error fails that
capability; partial Q3/Q4 success restricts the variant. Q6 counter problems are
conditional because a stable mapping may solve them. Always inspect per-trial
results and observations: there is no overall automatic approval to redesign.

## Recommendation after the run

Proceed with queue variant A only when Q1–Q6 support its advertised features in
the intended FLAC format, and Q7 if grouped playback matters. Require the owner
to confirm titles/art, clean joins in both rate directions, controls and the full
paced tail. If mixed rates alone fail, choose a fixed stream rate before writing
the queue bridge. If fundamental queue/live-delivery/control capabilities fail,
retain per-track title mode or the resampling approach from the earlier design.

Even an all-GO run leaves implementation work: finite item duration/length before
LMS finishes encoding, request-ahead backpressure, arbitrary seek/history access,
queue-to-LMS Next/Previous ownership, app seeking translated into LMS/core state,
ReplayGain/crossfade semantics across one-item-per-track boundaries, artwork
updates, grouping ownership, error recovery and the existing drift/elapsed model.
The probe generates its own audio; it does not claim to validate any of these.

Cleanup pauses and empties the test queue and verifies its length while the HTTP
server remains available, then closes the server. The original URI is not
restored automatically: it may refer to an obsolete bridge session or a queue
whose contents were deleted. Restart the room service and explicitly play from
LMS instead. Cleanup failures remain visible in the report and exit status.
SIGKILL, power loss and process crashes cannot guarantee cleanup. In that case,
stop playback/clear test items in the app before restoring the service. Shuffle
and repeat are not changed by the tool; if left enabled they invalidate the
assumed item order and finite test duration.

## Development verification

```bash
make
python3 tests/sonos_queue_probe_test.py
make test
```

The tests use loopback fake Sonos SSDP/topology/SOAP/event and LMS servers only;
no real speaker discovery or LAN traffic. They cover identity, stopped-LMS
preflight, queue DIDL/actions, exact lengths/ranges/request closes, paced
reconnects, all six steps, measurement bounds/resets/errors, real 4 Hz polling,
report verdicts, partial mutation cleanup, nonempty-queue refusal, interrupted
main execution, and owner-verified optional grouping. Independent libFLAC decoding
checks both sample rates and bit depths, sample count and PCM MD5. The probe
itself remains standard-library-only. There is no frontend to check.

## Optional Q8 and Q9

After the normal Q1–Q6 (and Q7 if requested), these flags add independent steps:

```bash
python3 scripts/sonos-queue-probe --replace-queue \
  --native-crossfade --length-mismatch
```

Q8 adds about two minutes. It saves coordinator GetCrossfadeMode, enables
SetCrossfadeMode=1 and verifies readback, then plays three 30 s WAV24 items at distinct 220, 440 and 660 Hz pitches.
Listen for two pitches at once, fading beeps, clicks, silence and the apparent overlap
length; enter that in the listening note. The report includes ordinary request
lead and `overlap_estimates` from the outgoing item's inferred natural end minus
the incoming item's inferred start. Bounds include two floored position clocks
and SOAP transit. This is a coarse position-handoff estimate, not an acoustic
measurement; early HTTP prefetch alone cannot establish crossfade. The original
mode is restored on completion/interruption. A failed restore remains visible in
report.md/events.log and is retried at cleanup; queue cleanup is attempted even
if restoring the setting fails. Restore it in the app if the report says pending.
Unsupported actions produce a failed Q8 result rather than a claim of support.
A GO supports only backup route B in [the queue design](sonos-queue-mode.md):
Sonos's undocumented fixed fade does not reproduce LMS smart/variable crossfade
or reveal the mixed samples needed for precise light timing.

Q9 adds about four minutes and deliberately serves invalid finite responses on
probe-owned, connection-close URLs. Four trials cover WAV and FLAC, each promising
20 s in DIDL, container header and HTTP Content-Length, but supplying 18 or 22 s
of frame-complete audio. A valid 20 s successor follows each fault. Full GET closes
after the actual body; bounded Range requests stay within the declared entity
and available bytes. A long body can be ignored at Content-Length or reset by
the client; it does not prove Sonos played the extra audio. The server records
promised bytes, available bytes and bytes actually accepted by its socket.
Listen/count beeps, note missing tails, errors, stalls/skips and whether the
successor starts gaplessly. Each trial has a separate successor listening answer,
positions, transport/error events, request/close logs and boundary estimates.

Q9's verdict is always **DIAGNOSTIC ONLY**, even if the speaker appears tolerant.
It measures consequences of a broken promise, not a valid production protocol or
permission to pad/truncate music. No claim of exact acoustic tail duration is
made without the listening observation. See the queue design's resolved-mixer
boundary policy; sealed lengths remain the baseline regardless of Q9 outcome.
Neither optional step has been run on the real speaker in this development round.

Field interpretation correction: after a WAV seek to 12 s in the 12 October
captures, Sonos requested offsets corresponding to approximately 53–62 s while
RelTime reported 12. The byte-to-file-time calculation is exact, but the fetch
horizon is not the audible seek target. Use fresh speaker position/events to
forward a seek to LMS. WAV's no-request Resume followed an already completed
response; FLAC reconnected. Retain byte history and support variable fetch speed
and reconnects, rather than depending on one persistent socket or a 5x limit.
