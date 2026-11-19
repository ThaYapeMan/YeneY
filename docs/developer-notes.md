# Developer notes

Moved from the README.

## Units

YeneY uses metric (SI) units only, in code comments, documentation, logs and reports:
metres, millimetres, kilograms, °C. Imperial units are not used.

## Sources for the Sonos local API

Additional connection-lifecycle sources (read for behaviour; nothing copied):

| Source | Licence or publication terms | Use |
| --- | --- | --- |
| [Architecture](https://docs.sonos.com/docs/connected-home-architecture) | Sonos documentation; no software licence stated | LAN API availability |
| [Control](https://docs.sonos.com/docs/control) | Sonos documentation; no software licence stated | Group addressing and command/event distinction |
| [Discover](https://docs.sonos.com/docs/discover) | Sonos documentation; no software licence stated | Household, group and player identities |
| [Subscribe](https://docs.sonos.com/docs/subscribe) | Sonos documentation; no software licence stated | Cloud subscription renewal and group changes |
| [Play audio (cloud queue)](https://docs.sonos.com/docs/cloud-queue-play-audio) | Sonos documentation; no software licence stated | Cloud queue context |
| [Connected Home Security](https://docs.sonos.com/docs/connected-home-security) | Sonos documentation; no software licence stated | Required S2 TLS suites |
| [aiosonos](https://github.com/music-assistant/aiosonos) | Apache-2.0 | Heartbeat, command deadlines, event dispatch and coordinator changes |
| [sonos-websocket](https://github.com/jjlawren/sonos-websocket) | MIT | Closed/reset connection handling |
| [node-sonos](https://github.com/sonn-audio/node-sonos) (formerly lox-audioserver) | AGPL-3.0 | Heartbeat, pending commands, reconnection and group refresh |

Each source above was read for behaviour; nothing copied. The public Sonos
pages describe cloud subscriptions and do not specify a local WebSocket idle
lifetime. Their three-day subscription limit is not evidence of a local
connection timeout. The client acknowledges both local subscriptions before
using the connection and renews them every 24 hours, on an expiry event, and
after reconnecting. Group-moved events and responses trigger a fresh local-info
lookup and subscriptions to the resolved group. Commands address the group ID,
not an individual player ID. The exact owned UPnP URI remains the audio identity
authority.

The reader retains partial frames and treats a control frame or temporary lack
of data as no application message yet. Commands retain their three-second
answer deadline. A client ping is sent after 30 seconds with no outgoing data;
a matching pong must arrive within three seconds. The first reconnect is
immediate; consecutive unsuccessful attempts wait 1, 2, 4, 8 and then 16 seconds.
An accepted position resets that failure streak. The last fitted line remains
available for at most three seconds from its last observation, with
`position_source_reason=reconnecting`; this is extrapolation, not a fabricated
speaker observation. Persistent failure still selects the whole-second source.

The check uses the room's worker, including its idle/event-reading path. It
reports ping, pong and close frames, connection age, negotiated TLS, offered S2
suites, and separate line fits for each item/seek segment. Local-info sockets
are closed before opening the WebSocket. The check holds at most one TLS socket
and one WebSocket; the capture reports measured held/peak connection counts in
`connections.json`. These counts describe that tool, not other clients.

The loopback fake permits three simultaneous connections: a room source, check
and capture. This does not establish the connection limit of physical speakers.
The node-sonos README's one-player-per-client-instance rule describes its client
object; it is not evidence of a speaker-wide one-connection limit. Each tool
closes only its own socket and sends no playback, topology or connection-reset
command. For initial owner measurements, run one observer at a time while the
room plays and retain the room journal; a hardware concurrency result requires
those observations.

Owner verification after deploying the committed fix (root on the YeneY
container; replace speaker addresses and unit names). Run each command as its
own step. Keep both rooms playing throughout, and introduce a track change and
a seek during each check so that multiple fit segments are recorded:

```sh
SINCE=$(date --iso-8601=seconds)
/opt/yeney/yeney --position-source-check --ip '<Port IP>' --seconds 180 > /tmp/yeney-Port-180s-check.txt 2>&1
/opt/yeney/yeney --position-source-check --ip '<Study IP>' --seconds 180 > /tmp/yeney-Study-180s-check.txt 2>&1
python3 /opt/yeney/scripts/sonos-local-api-capture.py --ip '<Port IP>' --seconds 180 > /tmp/yeney-Port-180s-capture.txt 2>&1
python3 /opt/yeney/scripts/sonos-local-api-capture.py --ip '<Study IP>' --seconds 180 > /tmp/yeney-Study-180s-capture.txt 2>&1
timeout 900 journalctl -u '<Port unit>' -u '<Study unit>' --since "$SINCE" --follow --no-pager > /tmp/yeney-15min-lifecycle-journal.txt
```

The capture archives are written under `/tmp`. Retain each archive's
`websocket-controls.jsonl`, `connection-ending.json` and `connections.json`.
The final command intentionally ends after 15 minutes with timeout status 124.
Expected: no `position source=upnp` during playing, no periodic connection
failure, and a line-fit restart at each item change or seek. A 60-second speaker
ping is a test model until the owner's control-frame capture confirms it.

The connection audit covers these separate readers and callers:

| Place | Silence or partial-frame handling |
| --- | --- |
| Production TLS HTTP setup | Bounded discovery/upgrade reads remain setup operations; their failure prevents readiness |
| Production WebSocket reader | Incremental nonblocking input; ping/pong returns without requiring another frame; TLS/TCP end is explicit |
| Room wait branch and command branch | Both use that reader; only command/subscription deadlines turn an absent answer into failure |
| Subscription setup/renewal | Both acknowledgements required; every pending command has its own deadline and is failed on connection end |
| Binary check | Uses the same room worker, rather than a separate request/receive loop |
| Capture reader | Incremental TLS/frame buffer; control frames, connection endings and partial data retained in the archive |
| Paired timing probe | Its separate blocking reader is called only while an explicit status command awaits a reply; its existing three-second command bound remains |
| Imitation speaker | Reads client frames until a complete frame arrives; handles client pings/pongs/close separately from commands; shutdown closes its sockets to release readers |

Use the owner's captured local API messages alongside the Sonos developer documentation:
[playbackStatus](https://docs.sonos.com/reference/playback-playbackstatus),
[playbackError](https://docs.sonos.com/reference/playback-playbackerror),
[metadataStatus](https://docs.sonos.com/reference/metadatastatus),
[groups objects](https://docs.sonos.com/reference/groups-objects),
[GlobalError](https://docs.sonos.com/docs/globalerror),
[Control](https://docs.sonos.com/docs/control), and
[Playback objects](https://docs.sonos.com/docs/playback-objects).
The captures take precedence for local wire shapes. Public documentation describes
cloud item identifiers; the captured local queue uses row number strings. The
previous-position table says string, while its example and the capture use a
number; accept both. Quality and local header fields absent from these public
pages remain capture-based extensions. BUFFERING precedes playback and holds
position. IDLE means neither playing nor paused, including startup, an empty
queue, a source that cannot pause, or a playback error. Events report changes,
including user seeks and track boundaries; steady progression is poll-only.

## Building and testing

The full gate uses `scripts/test-steps.json` as its only runtime step list.
`make test-serial` runs that list in the original order;
`YENEY_TEST_JOBS=1 make test` uses the same serial path. `make test` otherwise
uses one isolated group per available core; `YENEY_TEST_JOBS` overrides their number. The fast
gate remains serial: on 12 cores it took 220.5 seconds versus 249.5 seconds
with isolated parallel scheduling (the previous recipe took 213.8 seconds).

Shared binaries and core fixtures are built once before groups start. Pure
configuration tests that compile private fixtures run in a serial preparation
phase. Each parallel group has private loopback networking, IPC, shared memory,
`HOME`, `TMPDIR`, XDG directories and writable caches. The numeric user ID stays
the same. Bubblewrap is required. Groups have no route beyond loopback and read
the installed LMS and Perl dependencies from the shared cache without writing
there. Fixed `/tmp` and `/var/tmp` paths map to group-owned cache directories.

Checks that judge elapsed time, alignment, pacing, latency, speed or live soak
models run alone after every group has finished. The runner waits for the
container's cgroup processor use over 30 seconds to be below 15 percent of its
available cores and verifies conversion fitness of at least 20 times real time
before a full run and again before its quiet phase. Capacity is read afresh
from process affinity, effective cgroup cpusets and cgroup quotas. Host core
counts, load averages and host idle counters are not admission criteria.
Never start a gate while another round, pytest or a `make test` works in the
checkout, or while the consumer project's gate runs on the container.

`python3 scripts/test-runner.py --list` prints stable step names and commands.
Run one check serially on the existing binary with
`python3 scripts/test-runner.py --step NAME --no-build`. Failure output gives
a replay command that restores the manifest, test environment and isolation,
with a binary checksum, the group and the log path. Other groups
finish unless `YENEY_TEST_FAIL_FAST=1`. An interrupt terminates groups and their
descendants. Missing, duplicate, skipped or failed entries fail completeness;
the full gate also requires positive production `require_v1=True` evidence.

The terminal prints compact results in manifest order. The complete ordered
transcript remains in `combined.log`, with per-step logs and metrics in one
directory per group under the run directory printed at startup. Set
`YENEY_TEST_VERBOSE=1` for verbose output. The normal five-run retention policy
remains in effect. Scratch audio is removed; diagnostic text is retained.

To add a check, append a new stable name to the appropriate mode orders and add
its command, environment, mode membership, build-set references, phase and
reason to the manifest. Add a compilation rule to the Makefile if needed and
list its target in the referenced manifest build set. Do not add a second test
recipe. Commands retain Makefile-style dollar escaping. Mark `time_judging`
true and `phase` as `quiet` for a live-clock verdict; explain the reason. If
uncertain, use `quiet`. Simulated timestamp replays can use `parallel`; a
non-timing check that compiles a private fixture uses `serial`. Keep existing
names when inserting new checks.

```sh
make
make test
```

`make test` runs the bridge fixtures and bundled core suite without a physical speaker.
Persistent test installations use `$YENEY_TEST_CACHE` (default `~/.cache/yeney-test`);
per-run scratch uses `$TMPDIR` (default `/var/tmp`) and is removed on exit.
Only the last five transcript runs are retained in the cache. The cross-repository
SHM consumer requires the sibling `../LampaStream` checkout or an explicit
`LAMPASTREAM_CHECKOUT`; only `LAMPASTREAM_CONSUMER=off` remains available to the standalone core policy
tests; the full gate rejects a skipped consumer. See [test storage](../DEVICE-VERIFICATION.md#local-automated-test-storage-and-consumer-coverage).
Test tools also require `flac`, `ffmpeg`, and `clang-format` (set `CLANG_FORMAT`
to its executable path when needed). Physical
tests on real Sonos speakers use `scripts/device-test.sh`; see
[Testing and device-test tooling](how-it-works.md#testing-and-device-test-tooling)
and [DEVICE-VERIFICATION.md](../DEVICE-VERIFICATION.md).

The opt-in ReplayGain measurement runs with `SCENARIOS="9"` (also with `AUTO=1`),
without app prompts. It needs a ReplayGain-tagged track and the `flac` tool
(`apt-get install -y flac`). S9 measures the gain sent by LMS and the level change
in YeneY's streamed FLAC, temporarily changes the player's ReplayGain preference,
then restores it. `RG_TRACK` defaults to `id:47145` (Tony Bennett ft. Lady Gaga,
“The Lady Is A Tramp”, *Duets II*), specific to the owner's LMS library like the
`TRACK_A`/`TRACK_B` defaults. Find your own ID using the LMS CLI on port 9090:
`titles 0 20 search:The%20Lady%20Is%20A%20Tramp tags:alY`, then set
`RG_TRACK="id:<n>"`. A title instead selects the first of up to 20 matches with
ReplayGain and archives all matches in `s9-matches.txt`; none tagged means
INVALID with the matches listed. See the linked testing guide for `RG_SECS`.

Forgot `--recursive` when cloning? `git submodule update --init --recursive` fixes
it after the fact. Existing checkouts upgrading past the removal of the noson
backend should run this once after pulling:

```sh
git submodule deinit -f noson && rm -rf .git/modules/noson noson
```

The separate UPnP device-discovery PNG icon is generated by `python3 scripts/make-icon.py` using only the
Python standard library. The PNG and embedded header are committed, so building
does not need an image tool or a generation step.

## Player engine

The executable uses only yeney-core. `YENEY_PLAYER` is obsolete: unset or `core`
is silent; any other value produces one removal warning and continues with core.
The startup journal identifies it as `Player engine: yeney-core`.
The installer removes a room's `player.conf` only when it is precisely the
single `[Service]` / `Environment=YENEY_PLAYER=core` override (optional final LF).
Custom content and all other overrides, including `debug.conf`, are preserved.

## Source layout

| Path | What lives there |
| --- | --- |
| `yeney.cpp` | Startup: argument parsing, Sonos/LMS discovery, wiring the rest together. |
| `core_sonos.cpp` / `.h` | Core sink, bounded PCM feeder and audible clock. |
| `pcm_pack.h` | Project-owned signed PCM conversion. |
| `third_party/yeney-core/` | Sole player engine and recursive decoder notices. |
| `sbencoder.cpp` / `.h` | FLAC encoding of the decoded PCM, rate-limited relative to real time. |
| `sbstreamer.cpp` / `.h` | The HTTP server Sonos actually connects to for the audio. |
| `resume_state.h` | The "what does this unpause mean" decision logic, isolated from I/O for testing. |
| `stop_debounce.h` | The 400 ms stop/seek debounce logic, likewise isolated. |
| `sonos-status.cpp` / `.h` | UPnP polling of the Sonos device's own transport/track state. |
| `sonos-position.cpp` / `.h` | UPnP polling of actual playback position, exposed to the output thread. |
| `upnp/` | YeneY's own UPnP layer: discovery, SOAP control, GENA events, HTTP serving. |

## Further reading

- [How YeneY works](how-it-works.md): design, the Sonos pause problem and how
  it was found, metadata findings, the UPnP layer and test tooling.
- [The UPnP layer and wire fixtures](upnp-layer.md).
- [DEVICE-VERIFICATION.md](../DEVICE-VERIFICATION.md): which physical checks a change needs.

## Technical notes by subject

Grouped HTTP recovery and investigation: [Decisions and findings](grouped-range-recovery.md).

Grouped playback reserve: `YENEY_START_LEAD_MS=1000` (default; valid 0–10000; 0 retains legacy pacing). Set `YENEY_DEBUG=stream` for per-response lead and TCP diagnostics. See [decisions and device trials](start-lead.md).

The opt-in playback-clock probe is documented in
[Timing probe](timing-probe.md). Enable it with `YENEY_TIMING_PROBE=1`; it
never retimes audio. A locked clock now supplies continuous LMS position by
default; `YENEY_LMS_POSITION_FROM_MODEL=0` restores RelTime-only reporting. With
`YENEY_TIMING_PUBLISH=1`,
LampaStream can read the opt-in SHM timing contract; inspect it with
`scripts/yeney-timing-read <mac>`.

Queue audio is 24-bit with a 48 kHz speaker cap. Modern queue-configured
rooms advertise 192 kHz and yeney-core converts native local FLAC/ALAC with
an integer-ratio linear-phase FIR: 88.2/176.4 kHz → 44.1 kHz and 96/192 kHz
→ 48 kHz. Exact source counts give exact WAV lengths immediately, without
waiting for LMS/SoX to finish a conversion. Remote sources use the stream
path and the same conversion when necessary. Rates at or below 48 kHz pass
through unchanged. Ordinary stream-mode rooms retain LMS/SoX conversion
and their 48 kHz advertisement. Legacy `YENEY_AUDIO=16/44` retains the
44.1 kHz advertisement and LMS conversion. Unsupported rate families and speaker errors
stop/release queue playback and report failure to LMS. Queue diagnostics expose
`rendered_rate` for the current owned item. `YENEY_QUEUE_POLL_MS` defaults
to 500 ms; the relay checks every 100 ms during the first three seconds after
Play/Seek/track changes and the final three seconds of an audible item. GENA
events trigger fresh observations; successful position/transport polls are silent.

The queue device suite accepts `--remote <favourite name or episode URL>` to
choose its remote-source scenario. Feed/container entries are browsed for playable
episodes; candidates that produce no SlimProto start within five seconds are
unloaded, up to five attempts. If none is playable the scenario reports SKIP.
See [device verification](../DEVICE-VERIFICATION.md#remote-source-selection-in-the-queue-device-suite).

Room-level topology, ownership, SOAP and transport faults stop LMS once without
STMn or playlist advancement. Source/decode/length faults can fail a track;
a second consecutive fault within ten seconds stops the room. Continuous stream
display titles and durations follow LMS track changes; renderer DIDL is unchanged.

Queue items carry the plain sanitised LMS title, with artist and album in their
own musicTrack fields. An empty title stays empty; no non-empty queue fallback
is added. `YENEY_TITLE_FORMAT` applies only to the stream path, whose radio-type
DIDL makes the Sonos app display the title alone. Its artist-title default,
sent-title log and continuous-stream display formatting remain unchanged.

### Finite remote items

| LMS source | Path | Published length |
|---|---|---|
| Local file | Relay / Sonos queue | Existing exact rendered frame count |
| Remote, positive duration, `live_edge < 0` (or absent) | Relay, reason `remote-finite` | `ceil(duration × rate) + 5 × rate` frames |
| Live (`live_edge >= 0`) or no positive duration | Existing stream path | Continuous stream |

The named five-second margin is an upper bound promise, not silence padding.
If production ends early, the connection ends at the real audio end. If an
advertisement makes the episode longer, a continuation is inserted immediately
after the episode, before any following track, preserving every rendered sample.
Continuations share the title, artist, album and artwork and keep one LMS track
and continuous episode position. Overflow is held in the compressed spool until its exact remaining length is known, with a deadline ten seconds before the current segment ends. At the deadline an unfinished producer gets one generous upper bound: overflow already produced plus the parent full declared duration, capped by room quota and WAV limits. Only audio exceeding that bound gets another continuation. Startup never waits for episode completion.

All estimated items remain in the compressed spool for seek and resume after a
long pause. Estimated playback raises the default per-room quota to 8 GiB;
explicit `YENEY_QUEUE_DISK_MIB` still controls the quota. A measured three-hour,
44.1 kHz, stereo 24-bit incompressible episode needed 2,868,842,882 disk bytes
(2,857,680,000 PCM bytes). Current episode segments and the next item are retained. The budget accommodates
two such incompressible three-hour episodes during decode-ahead.
An explicitly inadequate quota fails with a logged storage fault; evicted history
and future-range timeouts close the connection without fabricated audio.

The generated-podcast regression uses the test-only `ffmpeg` encoder and the
existing private LMS 9.1.1 harness.

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

At the ten-second deadline, an unfinished producer gets its upper-bound fallback
even if no overflow is available yet. If the real source ends inside the parent
bound, the unused fallback closes at real EOF without padding, another LMS start
or a jump in the episode clock. Further items require an exceeded fallback bound.

LMS seeks within retained queue audio keep the same WAV and queue row for native,
unconverted ALAC, FLAC and PCM when the core proves the landing and the retained
prefix covers its exact overlap. Trimmed ALAC uses the original source origin
when attaching. Converted hi-res audio and AAC/MP3 with unprovable landings use a
single speaker switch with `landed-frame-unknown`; an exact PCM mismatch uses
`landed-pcm-mismatch` and preserves the retained WAV until that switch.

Check the production position client without playing anything or contacting LMS:
`yeney --position-source-check --ip <speaker-ip> --seconds 30`. This read-only
check prints discovery, upgrade and subscription results, the selected source,
and 5/20 Hz RTT and position-residual statistics. It can run beside a room service.
With `--link-rates`, the same read-only check measures 5, 10, 20 and 40 requests
per second. It reports the median, 95th percentile and maximum RTT for status
answers and WebSocket pongs. The baseline measures status and ping in separate
intervals. The interleaved trial sends a ping with each position question.
The displayed rate is the target cadence and the answer counts show how much of
it the link sustained.

The queue room reports `position_link`, `position_precision_ms` and
`position_trusted_answer_age_ms` beside `position_source`. The precision is
an estimate from the answer times of samples used in the fit. It is unavailable
when the current coordinate came only from an event. An uneven link keeps the
last fitted position advancing while answers are delayed.

Fallback room status includes `position_source_reason`; failed retries remind the
journal at most once per ten minutes. For redacted device evidence, run
`python3 scripts/sonos-local-api-capture.py --ip <speaker-ip> --seconds 60`
and attach the printed tarball. Change track and pause during that capture.

Queue mode can read the speaker's millisecond position from the Sonos local
WebSocket API on port 1443. This read-only interface uses a public application
key and the speaker's self-signed TLS certificate; it needs no Sonos account.
It is an undocumented local interface, so socket failures fall back to UPnP and
retry automatically. Room status shows `position_source` and
`position_precision`. Set `YENEY_POSITION_SOURCE=upnp` (or
`--position-source=upnp`) to force the previous position source.


The final TIMING-03b loopback comparison measured a -0.813 ms steady median over
120 seconds, all samples within ±30 ms, zero jumps and +0.003376 ms/s creep.
App seek, rewind, LMS in-item seek, natural boundary and row switch aligned in
101.308, 266.394, 107.455, 117.776 and 152.188 ms. Their 60-second median errors
were -1.092, -1.172, -0.859, +0.099 and -1.181 ms; all samples were within
±30 ms with zero jumps, and the largest absolute creep was 0.008924 ms/s.
This uses 20 Hz precise-source sampling and coalesced immediate core status
requests. The owner's two Port captures on 14 November had medians of +991
and +1047 ms for speaker position minus LMS time; the new loopback figures
await the owner device comparison. Full-gate acceptance was not achieved
within the round's three-attempt limit. See
[the position-source design](designs/speaker-position-source.md) for the
measurement table and retained historical failures.

`yeney --version`, the startup journal and room status (`playback.firmware`)
show the compile-time version `0.9.0-<bridge revision> (core <core revision>)`.
Builds read the release from `VERSION` and both revisions from Git. In LMS,
the player type is `yeney`. Its firmware field includes the speaker's shortened
version when available, for example
`0.9.0-<bridge revision> (core <core revision>; Sonos 86.10)`.
