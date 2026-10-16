# Device verification

## Device test levels

- **Level 0 — no physical test:** display, logging and installer changes. Run `make` and `make test` locally.
- **Level 1 — unattended:** for stream and pause logic changes, run `sudo env AUTO=1 scripts/device-test.sh` on the deployment host. It discovers the room's coordinator, sends AVTransport Pause/Play directly, allows up to ten seconds after resume for both playback clocks to start, then checks speaker and LMS time advance by at least three seconds within the next six seconds, checks continued position and track changes, and monitors transport status and journal errors. It prints a PASS/FAIL table and exits nonzero if any scenario fails. This measures playback progress; it cannot hear audio or inspect app dialogs.
- **Level 2 — real Sonos app:** occasionally run `sudo scripts/device-test.sh` and follow the existing app prompts, especially to confirm audible playback and app behavior. `QUICK=1` keeps this manual flow with shorter defaults.

AUTO and QUICK default to `SCENARIOS="1 2 5 6 7" S2_ROUNDS=1 LONG_PAUSE=30`;
explicit environment values override these defaults. Normal manual mode retains
scenarios 1–7, three S2 rounds and a 120-second long pause. AUTO requires Python 3,
`./yeney --list-rooms --details`, coordinator reachability on port 1400, LMS CLI
access and the room's journal; run it from the repository directory. The bridge
logs `speaker URI: stream=N session=<token>` independently of the displayed title.
Unknown or external URIs do not count as successful stream detection in AUTO.
Agents never SSH to or deploy on LXC 113; the owner runs physical tests there.


## Resume, double GET, and seek checks (physical Sonos required)

Build and test locally in WSL2 with `make` and `make test`. LXC 113 is deployment
only: the user pulls, builds, and restarts the service; agents never deploy or
SSH there. Capture a fresh journal throughout this single-track test:

```bash
journalctl -fu 'yeney@Sonos\x20Port.service' -o short-precise
```

A stream N is an HTTP delivery generation established by LMS `strm s`, not a
track. One track may legitimately use several IDs after rewinds, starts, and
seeks. URLs have `?session=<token>&stream=<N>` (or `&session=...` after existing
parameters); the random session is logged once at startup. After a bridge restart,
GET and HEAD of the previous URL, or a URL missing `session`, must return 404 with
`Content-Length: 0` and `Connection: close`, log `stale request: session ...`, and
produce no ACTIVE/STANDBY or device-resume activity. The current session still
serves audio normally. Re-priming a pause-ended response uses the same current URL and ID.

Verified on 2026-09-25: the native-FLAC relay test `120607` completed three
Stop-after-pause resumes with fresh FLAC GETs, status OK, and no reported dialog.
Bridge test `123027` verified LMS and Sonos-app pause/resume with `pause=stop`:
audio continued on the same stream with `strm u`, status OK, no dialog, and
approximately 0.9 s from Play to audio. The earlier `104917` frames/raw experiment
still reported ERROR_CORRUPT_FILE and dialogs. Evidence stays local in `logs/`.
Stop is now the default. The position regression observed in `123027` is addressed
by connection-based PCM anchoring; physically recheck position as described below.

1. **(a) LMS pause, rewind, play — defect I acceptance.** During playback,
   pause in LMS, rewind to the start while still paused, then press LMS play.
   Expect `strm q: superseded by strm s, no Pause` and
   `strm u: new stream pending from strm s, no same-URL resume`. Require exactly
   one new stream ID and one `PlaySqueezeBox` for that ID. There must be no
   PlayStream of the old URL, no `stream mismatch`, no empty GET response, and
   no error flag. Probe/real GETs for the new ID retain client-owned closure:
   the second GET must not make the server close the first.
2. **(b) LMS pause for 30 seconds, then resume — defect F acceptance.** Expect
   `strm p -> UPnP Stop (pause=stop)`, immediate silence, response closure, and
   `OK | STOPPED`. After 30 seconds press LMS play once. Without an open GET,
   expect `strm u after ended response -> PlayStream(same URL)` and a fresh
   FLAC GET for the same ID. Audio must continue without action in the Sonos
   app or a dialog. Repeat three times. Record LMS position before pause and
   after resume: it must continue from the paused position, not from zero.
   After ten seconds it should be approximately ten seconds further along,
   allowing for device startup and buffered-audio latency.
3. **(c) Seek by dragging the LMS bar — defect H acceptance.** During playback,
   drag forward and then backward within the same track. Each q/s burst should
   log `strm q: deferring Stop for 400 ms` followed by
   `strm q: superseded by strm s, no Pause`. Expect exactly one `Creating new
   stream (N)` per seek and probe/real GETs for it. Require playback at the new
   position without an intervening `UPnP Pause` or device stop. Separately issue
   a genuine LMS stop with no following s: expect
   `strm q -> UPnP Stop (400 ms elapsed, pause=stop)` after approximately 400 ms.
   With explicit `YENEY_PAUSE=pause`, expect UPnP Pause instead. The HTTP
   response ends immediately on q in both cases.
4. **(d) Sonos-app pause/play — defect J acceptance.** Test an immediate Play
   and a full 30-second pause. Expect `Device-initiated pause -> LMS pause`,
   `LMS CLI: <mac> pause 1`, `strm p -> UPnP Stop (pause=stop)`, and clean EOF.
   LMS must stay paused while Sonos reports STOPPED/OK; no restart or LMS play
   is triggered by STOPPED itself. Press Play in the app: the speaker moves to
   TRANSITIONING/PLAYING and opens a fresh GET. Expect one
   `Device-initiated resume: current stream N`, one `LMS CLI: <mac> play`, and
   `strm u: feeding held GET, no same-URL resume`. That GET gets a fresh FLAC
   header and chunked audio, with no 503, server-induced close, or second UPnP
   Play. Require status OK, continued audio and no app dialog. Verify LMS
   position continues from its pre-pause value even while Sonos RelTime is zero.
5. **(e) Status and startup isolation.** Require status `OK` throughout (a)-(d),
   with no `ERROR_*` flag at any point, including `ERROR_NO_PLAYABLE_CONTENT`,
   `ERROR_NO_RESOURCE`, `ERROR_LOST_CONNECTION`, and `ERROR_CORRUPT_FILE`.
   Require no `server error (500)` or `device transport command failed`.
   Also restart the bridge while Sonos plays
   its own radio, without starting LMS playback. Startup q commands must log
   `transport ignored before first bridge stream`; radio must not be paused.

Each GET has a unique request ID. For the current stream, exactly one GET is
ACTIVE; extras are STANDBY. Expect `stream N: GET #id ACTIVE` for the first
request and `stream N: GET #id STANDBY` for extras. STANDBY sends no headers or
audio and never replaces the active encoder. If Sonos closes a standby, expect
`standby closed by client` and no bytes written; ACTIVE must continue without
an audio gap. If ACTIVE ends by client close, idle timeout, send error, or
encoder error, the newest standby is `promoted` to ACTIVE with a fresh encoder,
HTTP headers, FLAC header, and audio. Verify both close orders. After 30 seconds
still on standby while ACTIVE streams, expect `standby timeout` and a silent
disconnect. A stream change redirects remaining standbys to the new URL (302)
and cancels the old active stream as before.

This replaces the initial import's (`f1eb715`) unsupported requirement that
both GETs receive headers promptly. Physical tests on 2026-09-24 showed Sonos
opening two same-stream GETs and closing either one within milliseconds; in one
case it closed the newer GET before our response arrived. The original upstream
README also records that rejecting the second request and serving the first
works. These observations support retaining the first active request and keeping
extras on standby until needed, rather than inferring ownership from socket state.

A **held GET** still means an ACTIVE request waiting for PCM while LMS is paused;
it does not mean STANDBY. In the default Stop mode a detected device resume feeds
that GET normally. It allows five seconds for play → LMS CLI → strm u → PCM,
then returns HTTP 503 if no audio arrives. Pause ends the active response and
silently disconnects existing standbys without promotion. Later GETs can wait
for resume audio on the same ID. With the explicit `YENEY_PAUSE=pause`
fallback, a detected device resume instead cancels an unfed held GET with 503
and reissues PlayStream with the same URL; active audio is never cancelled by
that hook. This fallback can reproduce the PAUSED FLAC-radio dialog.

Local tests exercise the real HTTP broker with simulated sockets and decode its
FLAC; they cover silent standby closure, uninterrupted active audio, promotion
with fresh FLAC after client close, idle timeout or send failure, newest-request
selection, stream changes, retained EOF state across reconnects, 30-second pause,
standby safety limits, idle limits, and q/s cancellation versus delayed genuine
stop. They cannot prove physical Sonos
status, audible stutter, or network command latency.

Resume decision table:

| State at `strm u`, in priority order | Sole action |
| --- | --- |
| New stream still pending after a paused `strm s`, including `p/q/s` | Let the decoded new-stream boundary allocate an ID and call PlaySqueezeBox |
| Open GET with default Stop mode, or ordinary LMS unpause | Feed that GET; do not issue PlayStream |
| Detected device resume with explicit Pause fallback | Cancel an unfed held GET, then PlayStream with the same URL |
| No open GET | PlayStream with the same URL |

Never more than one action per `u`. An ACTIVE GET must never end without audio
unless its client closes it, except for HTTP 503 after five seconds without PCM
or prompt cancellation of a speculative held GET before a SameURL resume.
STANDBY requests may disconnect silently on client close, pause, or the
30-second safety limit; obsolete stream IDs redirect as described above. An
ended-response flag must not override a pending new stream or an ordinary LMS
unpause into an open GET.

Local regressions cover paused seeks, response-before-Stop/Pause ordering,
STOPPED-to-PLAYING device resume, same-URL fallback, current GET feeding, and
connection position anchoring with stale/zero RelTime. The fallback's cancelled
held GET closes with HTTP 503 within 300 ms before a fresh same-ID GET carries
audio. Wrong-stream invalidation is ignored and active audio is preserved.
Repeat physical checks after changes; local tests cannot establish audible
continuity, app dialogs, or device timing.

**S7: LMS stop, then Sonos-app Play** (`SCENARIOS=7 scripts/device-test.sh`):
expect STOPPED and LMS mode stop after five seconds, with no automatic restart.
Press Play in the app: expect one LMS `play`, fresh FLAC audio on the held GET
(or a redirect to the new stream if LMS restarts the track), no 503, and no app
dialog. Record speaker state, LMS mode and now-playing after 15 seconds. S7 is
included in the default scenarios 1–7; the fix still needs physical verification.
LMS host selection is logged: explicit `LMS`, config, recent unit journal,
unit `ExecStart --server`, then the full unit journal.

S7 evidence from `sonos-test-20260925-160545` (9e47596) showed q-Stop worked,
but LMS's restart q ended held GET #58 with 503 before s allocated stream 11;
audio resumed with ERROR_NO_RESOURCE and an app dialog. Recheck with the fix:
the classified held resume must survive q/s, then log `held resume GET #N -> 302
stream M` or `closed by client`. Same-stream u logs `fed`. Only an unanswered
five-second hold logs `expired` and retains the prior 503 fallback. Local tests
reproduce the gap between q, s and ID allocation for both p-Stop and q-Stop;
physical status and absence of a dialog still require S7 on the speaker.

## S9 — measure ReplayGain in the streamed audio

S9 measures LMS's transmitted ReplayGain and the resulting PCM in YeneY's FLAC
HTTP response. It does not measure acoustic speaker output or Sonos volume.
It is opt-in, like S8; neither manual nor AUTO defaults include it. Both commands
run without app prompts:

```bash
sudo env SCENARIOS="9" scripts/device-test.sh
sudo env AUTO=1 SCENARIOS="9" RG_TRACK="id:123" RG_SECS=25 scripts/device-test.sh
```

`RG_TRACK` defaults to `id:47145` (Tony Bennett ft. Lady Gaga, “The Lady Is A
Tramp”, *Duets II*). Like the `TRACK_A`/`TRACK_B` defaults, this is specific to the
owner's LMS library; use an ID from your own library. To find one, send this to
the LMS CLI on port 9090 (the `Y` tag requests track ReplayGain):

```text
titles 0 20 search:The%20Lady%20Is%20A%20Tramp tags:alY
```

Set `RG_TRACK="id:<n>"` to use the returned ID as-is, or
`RG_TRACK="The Lady Is A Tramp"` to search up to 20 matches. S9 records every
match's ID, title, artist, album and ReplayGain (or `none`) in `s9-matches.txt`,
chooses the first match with ReplayGain, and logs why it was chosen. If none has
ReplayGain, S9 is **INVALID** and lists all matches. Explicit IDs also need a
track ReplayGain tag in LMS's library; missing metadata gives **INVALID**. S9 uses `songinfo ... tags:Y`
(`Y` is track `replay_gain`, `X` is album gain in
[LMS Queries.pm](https://github.com/LMS-Community/slimserver/blob/public/9.0/Slim/Control/Queries.pm#L5162)).
Install `tcpdump`, Python 3 and the FLAC command-line tool (`apt-get install -y flac`).
`NO_PCAP=1` is not supported for S9. Run from the repository on the bridge host;
the coordinator is discovered even in manual mode. Agents do not run this on the LXC.

S9 saves and logs the player's `playerpref replayGainMode`, then temporarily sets
it to **1 (track gain)** and **0 (off)**, in that order. Each run loads the same
track from its start, captures for `RG_SECS` seconds (default 25), then pauses.
The saved preference is restored and verified after the runs, on failures and
through the exit/interrupt trap. A failed restore is reported and retried at exit;
if LMS is unreachable, the report retains the value to restore manually.

Each run has its own full-payload `tcpdump -s 0` capture, filtered to LMS TCP 3483
and stream-port responses (1400–1409) to the coordinator. The ordinary 512-byte
capture is not used for this measurement. The stdlib-only Python helper
`scripts/replaygain_probe.py` reassembles TCP, reads the packed Slimproto `strm s`
gain, and selects the largest FLAC HTTP response. It removes HTTP chunking,
retains a truncated final chunk and decodes with `flac -d --decode-through-errors`.
The local LMS stream request's player ID selects the intended player; missing or
multiple matching starts are INVALID, rather than guessing among rooms/tracks.

The four reported checks are:

- Track-run sent gain versus the track tag, within 0.1 dB. A different gain is
  accepted only if LMS's calculated playback gain from `status` confirms it;
  both values and the adjustment are reported. Otherwise this check fails.
- Off-run raw gain equals zero (the Slimproto sentinel meaning no ReplayGain).
- Measured track-minus-off level equals the **sent** gain difference within
  0.5 dB, calculated as `10*log10(sum(track energy)/sum(off energy))`.
- The 10 ms mean-square envelopes align within ±6 seconds with Pearson
  correlation at least 0.95. The first second of each run is excluded, with at
  least three seconds of overlap. Insufficient, silent, constant or poorly
  correlated audio is INVALID, not evidence of a gain failure.

The table prints tag/sent/measured dB, correlation, alignment offset and both
peak dBFS values. Only all passing checks produce PASS; FAIL and INVALID exit
nonzero. Avoid changing tracks or player settings during the run. Existing DSP,
fades or clipping can affect the comparison; the captured evidence helps explain
such differences.

The tarball keeps `s9-track.pcap`, `s9-off.pcap`, extracted `.flac`, decoded `.wav`,
decoder logs, CLI replies, `s9-report.txt` and `s9-matches.txt` for title searches. Repeated S9 runs use separate
`s9#1`, `s9#2`, … directories. Local `make test` covers packet reconstruction,
16/24-bit WAV parsing, known-gain alignment, restoration and a real-FLAC round
trip when `flac` is installed; without it that fixture prints an explicit SKIP.
These synthetic checks do not replace a physical S9 run.

## Startup position and persisted drift

The owner deploys; agents never SSH to or deploy on LXC 113. Install initializes
`/var/lib/yeney`, without deleting learned drift, and restarts configured rooms.
Enable the timing probe and model reporting in the existing service environment.
A first-ever install has no prior; an upgrade may already have one. Journal
`timing-prior` confirms loading; `timing-lock` gives the true lock timestamp.
`position_phase=acquiring/prior/slewing/locked` and `slew_offset_ms` show reporting.
See [the clock contract](docs/timing-probe.md) for file format and expiry.

The user runs the following on LXC 113 after deploying. Queue two long tracks
in LMS on **Study (Sonos)** first: use two queued tracks of different PCM sample rates, for example id:32671
(48 kHz, library duration 591.090 s) then id:32678 (44.1 kHz). The 720 s capture includes enough
of the second track to measure phase reacquisition. Enable
PROBE and RAW in the existing service environment. These commands force a new
stream and start the checker within a few seconds of LMS playback. The runtime
service override changes only prior loading; it never deletes drift files.
The cold run can learn/save a mature rate for the subsequent warm run.

Deployment (from the installed checkout):

```sh
cd /opt/yeney
git pull --ff-only
git submodule update --init --recursive
make
sudo make install
```

Queue the two owner-library tracks once; the warm run uses the same playlist:

```sh
printf '94:9f:3e:fa:ba:66 playlistcontrol cmd:load track_id:32671\n94:9f:3e:fa:ba:66 playlistcontrol cmd:add track_id:32678\n94:9f:3e:fa:ba:66 stop\n' | nc -w 1 192.168.178.23 9090
```

Forced cold run:

```sh
COLD_SINCE=$(date --iso-8601=seconds)
printf '94:9f:3e:fa:ba:66 stop\n' | nc -w 1 192.168.178.23 9090
sudo install -d /run/systemd/system/yeney@Study.service.d
printf '[Service]\nEnvironment=YENEY_TIMING_PROBE=1 YENEY_TIMING_RAW=1 YENEY_TIMING_PRIOR=0\n' | sudo tee /run/systemd/system/yeney@Study.service.d/90-field-prior.conf
sudo systemctl daemon-reload
sudo systemctl restart yeney@Study.service
printf '94:9f:3e:fa:ba:66 playlist index 0\n94:9f:3e:fa:ba:66 play\n' | nc -w 1 192.168.178.23 9090
/opt/yeney/scripts/yeney-lms-position-check 192.168.178.23 94:9f:3e:fa:ba:66 --seconds 720 > /tmp/study-cold-start.txt
sudo journalctl -u yeney@Study.service --since "$COLD_SINCE" --no-pager -o short-precise > /tmp/study-cold-journal.txt
```

Warm run (same playlist and timing settings; prior loading restored):

```sh
WARM_SINCE=$(date --iso-8601=seconds)
printf '94:9f:3e:fa:ba:66 stop\n' | nc -w 1 192.168.178.23 9090
printf '[Service]\nEnvironment=YENEY_TIMING_PROBE=1 YENEY_TIMING_RAW=1 YENEY_TIMING_PRIOR=1\n' | sudo tee /run/systemd/system/yeney@Study.service.d/90-field-prior.conf
sudo systemctl daemon-reload
sudo systemctl restart yeney@Study.service
printf '94:9f:3e:fa:ba:66 playlist index 0\n94:9f:3e:fa:ba:66 play\n' | nc -w 1 192.168.178.23 9090
/opt/yeney/scripts/yeney-lms-position-check 192.168.178.23 94:9f:3e:fa:ba:66 --seconds 720 > /tmp/study-warm-start.txt
sudo journalctl -u yeney@Study.service --since "$WARM_SINCE" --no-pager -o short-precise > /tmp/study-warm-journal.txt
cat /tmp/study-cold-journal.txt /tmp/study-warm-journal.txt > /tmp/study-journal.txt
```

On the Proxmox host, pull to `/tmp`:

```sh
pct pull 113 /tmp/study-cold-start.txt /tmp/study-cold-start.txt
pct pull 113 /tmp/study-warm-start.txt /tmp/study-warm-start.txt
pct pull 113 /tmp/study-journal.txt /tmp/study-journal.txt
```

Expected per track (a rate change creates a new stream):

| Metric | First cold track | First warm track | Following different-rate track |
|---|---|---|---|
| Stream time to lock | 60–63 s (60 s measured span gate) | <=15 s; prior-load log required | <=15 s from new-stream start; live drift carried |
| Before lock | within RelTime quantisation, about ±500 ms + RTT | same, prior rate | unity/prior progress until phase lock |
| Convergence | normally <=5 s | normally <=5 s | normally <=5 s |
| After-slew error | p95 <=5 ms, aim max <=5 ms | same | same; inspect isolated jitter outliers |
| 250 ms steps | <=400 ms during slew + jitter | same | about 250 ms + jitter |
| Excess step | <=150 ms during slew + jitter | same | near zero + jitter |
| Flat interval / backwards | 0 / 0 | 0 / 0 | 0 / 0 after audible start; no repeated 1 ms reports |

The explicit `boundary_window` covers the last old-track STAT before the PCM
boundary through ten seconds after the new STMs. Require `old_overrun_ms=0`,
`largest_step_excess_ms<=100`, `backwards_count=0`, and `longest_flat_s<=0.5`
unless listening confirms a real audible gap. The October 12 local replays
show cold/warm excess 0/0 ms, no backwards samples, and 0.25 s longest flat.
These are captured-response simulations, not new device measurements. Track 1's
submitted PCM end is 28,371,728 / 48,000 = 591.077667 s (wire cap 591077 ms),
about 12 ms below LMS's library duration. STMs for the restarted track should
arrive just after a fresh eligible PLAYING zero following TRANSITIONING, with
a small positive elapsed that advances on subsequent timers; it must not
start at 2.7–2.85 s or repeat that value. If a zero cannot be qualified, a
positive reply seeds its conservative elapsed interval instead.

Overall aggregates each track; it must not show the former boundary jump/freeze. A2 automated lock times remain 60/10 s; the new
field-informed cold replay now locks at 61.222 s (62.227 s before this change). These are position/clock targets,
not acoustic guarantees. If a valid prior was not saved during the cold run,
the warm run must report a cold lock rather than pretending it loaded one.
`time_to_lock_s=unknown` means stream/lock journal evidence is missing; inspect
the captured journal. The runtime override disappears on reboot; the warm
value `1` is the default. Restore your usual PROBE/RAW settings after testing.


The separate core pin is `946047c`. Deploy with
`git submodule update --init --recursive` so Apple ALAC is present. YeneY's
unpaced Sonos analysis tap deliberately leaves the core's optional play-time
schedule unset; LampaStream should keep using the Sonos `/yeney-timing-<mac>`
audible mapping. Standalone paced yeney-core players can supply the new schedule.

After both runs, remove the test override and restore the normal unit settings:

```sh
sudo rm /run/systemd/system/yeney@Study.service.d/90-field-prior.conf
sudo systemctl daemon-reload
sudo systemctl restart yeney@Study.service
```

The strict requested cold <=60 s from STMs is not claimed: the unchanged cold
lock gate requires 60 s between the first and latest measured edges. Expect
about 62 s from stream 2 start with this field timing. Preserve pre-lock constant
phase without drift (the previous -165/-218 ms are capture-specific, not
calibration targets), after-lock p95 <=5 ms, and no startup reset cycle on track 2.
Inspect the explicit track-change sample gap and grouped anomaly output;
`audible_gap_s=unknown` requires listening to assess acoustic silence.


For the short listening check, run on the deployment host after restoring the
normal service environment. Load both tracks afresh, start, then seek near the end:

```sh
printf '94:9f:3e:fa:ba:66 playlistcontrol cmd:load track_id:32671\n94:9f:3e:fa:ba:66 playlistcontrol cmd:add track_id:32678\n94:9f:3e:fa:ba:66 playlist index 0\n94:9f:3e:fa:ba:66 play\n' | nc -w 1 192.168.178.23 9090
sleep 3
printf '94:9f:3e:fa:ba:66 time 575\n' | nc -w 1 192.168.178.23 9090
```

Listen through the change (about sixteen seconds later). Note any silence or
truncated tail separately from LMS position. Seeking creates an explicit new
coordinate and this short run is not a lock-time or drift acceptance test.
No audio/transport change is intended; a physical gap or a cut-off tail needs
its own follow-up before calling the transition acoustically seamless.

## Queue mode phase 2: LMS read-ahead

Owner-run only: no Sonos, room-service or room-configuration access. The extra
player is **Queue Test (YeneY)**, raw MAC **02:59:51:54:00:01**. Reserve this
identity for the diagnostic; only one instance may run. Agents never SSH to or
deploy on LXC 113. Root's PATH there lacks `/usr/local/bin`, so always invoke
`/usr/local/bin/yeney-queue-dryrun` by its full path.

On LXC 113, the owner builds and installs the pushed revision:

```bash
cd /opt/yeney
git pull --ff-only
git submodule update --init --recursive
make
sudo make install
```

Then define this helper once. It copies all three PID-stamped artifacts
from the exact invocation whose Report line was captured, never from a wildcard
or a different run:

```bash
run_queue() (
    trap ':' INT
    name="$1"
    /usr/local/bin/yeney-queue-dryrun --server 192.168.178.23 --seconds 1800 | tee -i "/tmp/dryrun-${name}.console"
    source_base=$(sed -n 's/^Report: \(.*\)\.md$/\1/p' "/tmp/dryrun-${name}.console")
    test -n "$source_base" && test -f "${source_base}.md" || return 1
    cp -- "${source_base}.md" "/tmp/dryrun-${name}.md"
    cp -- "${source_base}.tsv" "/tmp/dryrun-${name}.tsv"
    cp -- "${source_base}.log" "/tmp/dryrun-${name}.log"
)
```

Use the LMS address alone, never append `:9000`. Perform these four separate
runs. **After each invocation registers the player, while the tool is running,
press Stop on Queue Test (YeneY) and clear its playlist before loading new
music.** This prevents a reconnect from resuming an earlier playlist at an
unknown offset. Start each new playlist at index zero; do not enable shuffle,
repeat or edit it during a measurement. The dry-run compares the zero-based
playlist occurrence sequence after Stop/flush with LMS's displayed index.

1. **ALAC, crossfade off:** run the command below, then stop/clear the test
   player, disable crossfade, load a local ALAC album and press Play.

   ```bash
   run_queue alac-off
   ```

2. **ALAC, crossfade 5 s:** start a fresh invocation, stop/clear, enable 5-second
   crossfade, load an ALAC playlist from different albums (so the smart album
   rule does not suppress overlap) and press Play.

   ```bash
   run_queue alac-on
   ```

3. **FLAC, crossfade off:** fresh invocation, stop/clear, disable crossfade,
   load a local FLAC album and press Play.

   ```bash
   run_queue flac-off
   ```

4. **FLAC, crossfade 5 s:** fresh invocation, stop/clear, enable 5-second
   crossfade, load a FLAC playlist from different albums and press Play.

   ```bash
   run_queue flac-on
   ```

Use tracks long enough to measure 12 seconds of successor lead. Let at least two
joins complete in every run. Ctrl-C or the 1,800-second limit ends that invocation
and writes its Markdown, TSV and log; wait for the helper's copies before
starting another run. This simulates audibility at 1.0x and makes no sound.
Observe LMS's displayed track and time as well. Record any unexpected initial
stream, skip, fault, stall or changed transition setting. Optional WAV/AIFF/MP3
runs use the same procedure. For known upstream transcoding add `--transcoded`
to the full-path invocation; MP3 counts remain unknown and publish sealed.

From the Proxmox host copy the fixed, matched names to `/tmp`:

```bash
pct pull 113 /tmp/dryrun-alac-off.md /tmp/dryrun-alac-off.md
pct pull 113 /tmp/dryrun-alac-off.tsv /tmp/dryrun-alac-off.tsv
pct pull 113 /tmp/dryrun-alac-off.log /tmp/dryrun-alac-off.log
pct pull 113 /tmp/dryrun-alac-on.md /tmp/dryrun-alac-on.md
pct pull 113 /tmp/dryrun-alac-on.tsv /tmp/dryrun-alac-on.tsv
pct pull 113 /tmp/dryrun-alac-on.log /tmp/dryrun-alac-on.log
pct pull 113 /tmp/dryrun-flac-off.md /tmp/dryrun-flac-off.md
pct pull 113 /tmp/dryrun-flac-off.tsv /tmp/dryrun-flac-off.tsv
pct pull 113 /tmp/dryrun-flac-off.log /tmp/dryrun-flac-off.log
pct pull 113 /tmp/dryrun-flac-on.md /tmp/dryrun-flac-on.md
pct pull 113 /tmp/dryrun-flac-on.tsv /tmp/dryrun-flac-on.tsv
pct pull 113 /tmp/dryrun-flac-on.log /tmp/dryrun-flac-on.log
```

Original artifacts are `/tmp/yeney-queue-dryrun-<timestamp>-<pid>.{md,tsv,log}`.
The temporary FLAC history in `/tmp/yeney-queue-dryrun/QueueTest` is deleted on
normal exit; `--keep` retains it until the next invocation. Copy retained
history before another run. Nothing is written to `/var/lib/yeney`.

Report times are monotonic seconds; `-1` means unobserved. Each occurrence
records wire codec (FLAC/ALAC/AAC/MP3/PCM), strm-to-audible startup delay, decoder
completion, STMs/STMd, transition, overlap, promised/actual frames and faults.
The first progressive exact-count FLAC/ALAC item should become audible and send
STMs within two seconds of its strm. Unknown counts remain sealed. A
`no-transition` boundary is correct with crossfade off. `successor-deadline`
means the overlap wait exhausted its audible safety bound; collect the log.
`sealed-startup` explicitly preserves an unknown-count source's full tail without
overlap when sealing must precede its first audible start.

The CLI on port 9090 is polled once per second with the literal MAC for playlist
index, time and title. TSV parallel pipe-separated columns retain all samples;
titles retain LMS percent encoding. `lms_matches_audible` is false when any
sample disagrees, true when all samples agree, and unknown without usable CLI
evidence. More than two seconds of continuous disagreement fails with
`lms-track-mismatch`; isolated boundary samples are counted but tolerated.

**LMS read-ahead OK** requires at least one completed predecessor/successor pair,
all completed leads at least 12 seconds, no storage fault and no sustained LMS
track mismatch. Incomplete rows are reported separately as run-end,
source-fault or flushed. A source fault is recorded without turning unfinished
rows into negative leads. These checks do not qualify acoustic gaplessness or
Sonos controls, which remain later phases.
