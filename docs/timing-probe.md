# Sonos playback clock and LampaStream timing contract

The locked measurement model now supplies continuous LMS-visible audible
position by default. It never changes playback, transport, clocks, audio,
the audio tap or timing publication. CLOCK_MONOTONIC is the time base; YeneY and LampaStream must
share the host kernel and make both SHM objects visible in the reader's
filesystem. The owner performs deployment and acoustic tests.
With PROBE unset/0, traffic, audio and output remain unchanged from 4641b46.

| Setting | Accepted values | Default |
|---|---|---|
| `YENEY_TIMING_PROBE` | `1` enables coordinator measurements; unset/`0` disables | off |
| `YENEY_TIMING_RAW` | `1` logs requests and brackets, only with PROBE | off |
| `YENEY_TIMING_STALE_S` | decimal seconds, 10–600 without a usable edge | 60 |
| `YENEY_TIMING_LOCKED_EVERY_S` | decimal seconds, 1–60 between locked edge probes | 5 |
| `YENEY_TIMING_PUBLISH` | `1` enables the timing object and existing core audio tap, only with PROBE | off |
| `YENEY_LMS_POSITION_FROM_MODEL` | `1` uses a qualified locked model for LMS position; `0` restores RelTime-only reporting | 1 |
| `YENEY_AUDIBLE_OFFSET_MS` | signed decimal integer, -500–500, added to published audible time | 0 |

Invalid values warn once and fall back to defaults. Signs and whitespace are
rejected for unsigned settings; calibration accepts a minus sign but not plus.
Leading zeros are accepted. Extra settings are not inspected with PROBE off;
calibration is inspected only with PUBLISH on. PUBLISH without PROBE does
nothing. PROBE on with PUBLISH off still uses the improved measurement and
locked polling; it creates no timing/audio object.

## Bracket clock

Only a known coordinator, PLAYING our exact current stream URI, with LMS
unpaused and source ownership retained, measures. Members publish state none.
Pause, flush, seek/new stream, reconnect/new PCM anchor and loss of eligibility
invalidate the epoch. Same-rate gapless boundaries and canonical HTTP Range
continuations keep it. Models survive missing edges until STALE expires, measured from the last
usable bracket (or activation before the first bracket), not from midpoint
regression acceptance. Thus midpoint outliers cannot expire a good contract.

The separate worker bypasses the normal one-second position lease; its replies
never update that cache. Send/full-response CLOCK_MONOTONIC timestamps and
RelTime define each adjacent n -> n+1 bracket. RTT rejection, 200 ms deadline,
2/4/8/10 s error backoff and rolling limits (8 requests/1 s, 30/10 s, across
resets) remain unchanged. RTT rejection starts at 80 ms, then uses twice the
128-sample p95 clamped to 10–80 ms. Normal leased reads can seed acquisition
without becoming fitted observations or additional requests.

The published clock fits `t(n) = t0 + k*(n-n0)` inside the largest number of
brackets in the last 600 stream seconds, at most 1200 brackets. Wide brackets
remain constraints: their midpoint need not be an accurate observation. For
each slope, interval intersection or an endpoint sweep finds the maximum
consensus offset band. Search is bounded: previous slope +/-200 ppm, 10 ppm
coarse steps, then 1 and 0.1 ppm refinement around consensus endpoints. Choose
the middle of the feasible slope range and the middle of its offset band. If
slope solutions are disconnected, choose the nearest maximum-consensus sampled
solution. Equal-count disconnected offset bands prefer the widest supported
band, whose centre is used. `band_ms` is the full offset band **conditional on that chosen slope**,
evaluated at the newest edge; it is not the union over every possible slope.

Without a prior, lock still requires >=60 s span, >=40 brackets, band <=15 ms
and <=2% violations. The Study A2 epoch 15 fixture reaches a <=15 ms phase
band after 11 edges / 10 s, but originally waits for the 60 s span. Drift fitting
still requires 120 s. With a qualified prior, phase evidence alone (at least
five brackets, the **same** <=15 ms band and <=2% violations) can lock; A2 locks
at 10 s. The prior does not preserve phase. Its slope uncertainty contributes
to uncertainty growth; LMS still rejects uncertainty above 10 ms. Other Study
B epochs reach phase precision in 4–20 s, before the normal 60 s gate.

The slope search and live 120 s drift fit remain unchanged. A prior fixes the
acquisition slope, then the live fit validates it. More than 2% violations after
ten observations, two consecutive locked contradictions, or a 120 s live slope
range excluding the prior by over three stored sigmas discards it, logs
`timing-prior dropped=live-edge-contradiction`, invalidates its disk record, and
returns to the original acquisition gates. Drift survives epochs in memory.

`make install` creates `/var/lib/yeney` (0755). Each speaker has
`drift-<hex-encoded-UDN>.state`; the topology UUID is the speaker's UDN without
the `uuid:` prefix. Version 1 stores time-slope ppm, sigma ppm and a Unix
REALTIME timestamp, never phase or monotonic timestamps. Accept records at most
seven days old, not future-dated, with |drift| <=500 ppm and 0<sigma<=15 ppm.
A live drift needs the original 120 s fit and 60 s of qualified locked drift before being
saved. Sigma is the feasible slope range divided by sqrt(12), floored at 0.1 ppm.
Writes need a >=1 ppm drift/sigma change or daily timestamp refresh, are limited
to once/hour, and flush pending changes on clean shutdown. Files use unique
same-directory temporary files, fsync and atomic rename. Missing/read-only
storage warns once and playback continues; no runtime directory creation is
required. Service processes run under the existing systemd identity.

Before lock published uncertainty is unknown; after lock it is half the chosen
band plus a 1 ms guard. This is a counter-based bound, not a certified acoustic
or DAC bound.

Locked probing targets one tick every LOCKED_EVERY seconds, with up to three
samples at prediction -H/0/+H, with H = clamp(RTT p95 + send-latency p95
+ 5 ms, 20 ms, 60 ms). Median RTT/2 and the rolling median first-send
lateness are subtracted from the planned burst centre. Send lateness is actual
first send minus its planned deadline, over up to 120 bursts. Three requests
are the maximum per burst; the existing 8/1 s and 30/10 s limits and SOAP-error
backoff remain independent of epochs.

An all-new burst contributes `edge <= first send` (`upper-bound`); an all-old
burst contributes `edge >= last receive` (`lower-bound`). Raw timing-edge lines
retain these literal bounds, with the open endpoint printed as `inf` or `-inf`
and midpoint/half-width/residual as `nan`. The bounding sample's RTT supplies
its observation width. For consensus, one-sided endpoints include tolerance
max(2 ms, RTT/2), also used for contradiction detection. This accounts for
SOAP evaluating RelTime during the request: the supplied field fixture has
new-second replies whose send precedes the feasible tick by about 4 ms.
Two-sided brackets keep their original endpoints.

Lose lock only after two consecutive observations lie outside the predicted
band by more than max(2 ms, half their observation width). Missing, rejected or
failed replies alone cannot contradict it. A first contradiction is held out
pending confirmation. A consistent one-sided bound keeps the mapping locked.
After an unbracketed burst, retry at the next second up to three times, then
resume the configured normal interval. A two-sided hit clears this retry count.
Published uncertainty adds 0.1 ms per second since the last two-sided bracket,
plus half the feasible slope range times that age. Steady publication continues
at <=1 Hz while lock holds. The existing stale-observation expiry remains.


## Continuous LMS position

`YENEY_LMS_POSITION_FROM_MODEL=1` (default) uses a smooth audible reporting
coordinate whenever PROBE is enabled; it does not enable PROBE or PUBLISH.
Setting `0`, or PROBE off, keeps the original RelTime reporting. Transport,
encoded handoff coordinates, delivered PCM, pause holds, handed-frame caps and
track-boundary accounting are unchanged.

Reporting phases (`position_phase` in timing lines and `position-phase` events):

| Phase | Meaning |
|---|---|
| acquiring | Extrapolate latest accepted RelTime with MONOTONIC, unity slope |
| prior | Same extrapolation using the qualified stored rate |
| slewing | Qualified locked model, bounded convergence from reported position |
| locked | Report model position; convergence finished |

Acquisition uses the middle of the one-second RelTime interval (integer +0.5 s)
and its actual normal SOAP midpoint when available. Repeated leased values do
not refresh its timestamp. Between observations position advances locally;
observation corrections also slew. Before the first accepted position the
existing anchor/start safeguards apply. With no phase evidence, the first
integer cannot reveal its fractional second: expect up to roughly 500 ms
initial phase error, now smooth instead of a staircase.

The locked model is inverted in the existing ConnectionPosition PCM coordinate,
including calibration if publication is enabled. It must match stream/base/rate,
be fresh under STALE and have finite uncertainty <=10 ms; active publication
must be locked. The model remains bounded to the aged latest RelTime interval.

Each report advances the previous coordinate by monotonic elapsed time, then
corrects at at most 15% of real time (0.85–1.15x nominal). A 650 ms error takes
4.34 s; 750 ms takes 5 s. Corrections greater than 750 ms jump immediately as
discontinuities. Explicit seek/new-stream/reconnect resets discard the slew
state; ordinary switches retain the audible high-water mark. `slew_offset_ms`
is reported position minus target. No ordinary switch moves backwards. Pause,
unlock, stale model, flush, new stream and reconnect retain the existing model
eligibility/fallback fences; same-rate gapless boundaries retain continuity.

The owner field check retains its arguments and original sample/regression
output:

```sh
scripts/yeney-lms-position-check <lms-host> 94:9f:3e:fa:ba:66 --seconds 600
```

It still sends only `<mac> time ?`, every 0.25 s, without catch-up bursts.
Additional summary fields report producer time to lock from the first sample,
largest position step after ten seconds, step in excess of elapsed time, and
backwards count/magnitude. It reads the **local** journal for `timing-lock`
events matching the speaker RINCON UDN derived from the MAC. Run on the YeneY
host with journal read permission. Lock is `unknown` if producer events are
unavailable: smooth positions cannot independently identify estimator lock.
Errors before/after lock and after the five-second slew allowance are relative
to the final stable linear fit, not independent Sonos/acoustic ground truth.
Use one item longer than the check; LMS track-time resets are legitimate discontinuities.
The original whole-run residuals and ppm remain for compatibility. The after-lock
statistics include the convergence transient; use `after_slew_error` for steady
precision. Keep playback continuous without pause, seek, skips or repeats.

Expected Study A2 replay: cold lock 60 s; prior lock 10 s; no backwards samples;
250 ms steps <=287.5 ms plus scheduling/timestamp jitter. For the physical Study,
aim for cold lock about 60–65 s, prior about 5–25 s (phase precision depends on
brackets), steady errors <=5 ms (previous field results about 1.5 ms), initial
errors <=500 ms plus SOAP/lease uncertainty. Slewing removes the lock jump but
its first post-lock error can still equal the initial offset. These are field
acceptance targets, not acoustic guarantees.

## Audio frame correspondence

4641b46's Sonos sink did not instantiate the bundled core SHM sink. Opt-in
PUBLISH now instantiates that **unchanged** sink alongside the Sonos sink in
YeneY code; no submodule modification, PCM processing change or ABI change is
needed. Each accepted, decoder-normalized/gained/faded stereo frame is passed
to both sinks exactly once. Sonos encoding retains its original 24/16-bit
packing; the analysis tap retains the existing SHM sink's 16-bit quantization.
The tap never paces or supplies playback/elapsed-time feedback.

For an accepted batch, record the core SHM generation and committed
`abs_write_pos` before/after export. The delta must equal the accepted stereo
frame count. The feeder records its existing stream-relative first frame
`core_first - streamBase` and the corresponding SHM absolute first frame.
Within that affine run:

```
shm_frame = abs_first + (stream_frame - stream_first)
stream_second = (stream_frame - ConnectionPosition.base) / stream_rate
```

The probe's second zero is the connection/PCM anchor, not an LMS track position
or a FLAC byte count. The publication anchor maps the later of that base and the current affine
run's first stream frame; its audible time is evaluated in the connection
coordinate. This also supports an export gap or generation change midway
through a connection. The producer can export ahead of the feeder: continuity permits
prediction of subsequent accepted frames until explicitly invalidated.
Gapless same-rate batches extend the same run. Finite canonical Range resumes
reuse the original response's bytes and PCM anchor, so they do not reset it.
A plain reconnect or Range restart gets a new connection/PCM anchor and epoch.
Pause/flush may rewind the core coordinate, but SHM absolute frames continue;
new streams and rate changes acquire a new run. YeneY has no local resampler: core advertises the Sonos sink's maximum
rate to LMS and rejects decoder formats above that limit. Any required LMS
transcoder resampling happens upstream. Both sinks receive the same accepted
output frames, so there is no conversion ratio between their coordinates. The
tap rate is explicitly the existing Sonos encoder rate, including legacy audio
mode; this does not change that mode's playback or LMS rate accounting.

The core sink can skip an export if its nonblocking lock is busy. YeneY detects
the missing committed delta, immediately invalidates the timing contract before
another exported batch, and reacquires a new affine run. It never treats lost
exports as contiguous samples. New track/rate invalidation happens before the
new PCM export; generation/run changes invalidate publication before a new
mapping. Old ring samples must not be interpreted using a later epoch. Invalidation
retires every frame up to the latest committed producer cursor, including
batches still waiting for the feeder; a new lock only accepts the newer range.

## Timing object, version 1

POSIX name `/yeney-timing-<mac>`, Linux path `/dev/shm/yeney-timing-<mac>`.
MAC formatting is the audio object's lowercase colon-separated six bytes,
e.g. `48:a6:b8:20:39:64`. Fixed **128 bytes, little endian**, 8-byte aligned:

| Offset | Bytes/type | Field |
|---:|---|---|
| 0 | 4 bytes | magic `YNTM` |
| 4 | u16 | version = 1 |
| 6 | u16 | flags: bit 0 = frame bounds present; other bits reserved |
| 8 | u32 | write_seq: odd during write, even when stable |
| 12 | u32 | size = 128 |
| 16 | u64 | shm_generation, matches audio v1 extension |
| 24 | u64 | model_epoch |
| 32 | u32 | state: 0 none, 1 acquiring, 2 locked, 3 stale |
| 36 | u32 | discontinuity flags: bit 0 pause, bit 1 discontinuity |
| 40 | u64 | anchor_abs_frame (stereo-frame SHM coordinate) |
| 48 | u64 | anchor_audible_mono_ns, calibration already applied |
| 56 | u32 | sample_rate_hz |
| 60 | i32 | drift_ppb, **audio-rate** drift |
| 64 | u32 | uncertainty_us; UINT32_MAX means unknown |
| 68 | i32 | offset_us, calibration applied |
| 72 | u64 | updated_mono_ns |
| 80 | u64 | valid_from_abs_frame, inclusive |
| 88 | u64 | valid_until_abs_frame, exclusive; UINT64_MAX for continuous run |
| 96 | 32 bytes | reserved, zero |

```
audible_ns(N) = anchor_audible_mono_ns
              + (N-anchor_abs_frame)*1e9
                / (sample_rate_hz*(1+drift_ppb*1e-9))
```

Subtract frames using signed arithmetic. `abs_write_pos` is the boundary just
after the newest exported frame and can also be predicted. Calibration is
already in the anchor: do **not** add offset_us again. The measured time-slope
drift is `(k-1)*1e6`; the channel uses `(1/k-1)*1e9`, the reciprocal audio-rate
drift required by this formula. Thus the Study time slope +10.15 ppm corresponds
to about -10150 ppb in the channel.

Writer uses atomic 32-bit words between odd/even release-compatible sequentially
consistent sequence updates. Reader reads even sequence, copies 128 bytes,
then rereads and requires identical even sequence. Read audio extension with
its own seqlock and require matching generation/rate. Reread timing sequence
after the audio snapshot: accept only the same sequence/epoch. Require locked
state, a frame inside the validity bounds and a fresh updated_mono_ns. The owner
tool rejects heartbeats older than 3 s. State changes publish immediately;
otherwise updates are at most once/s. Loss of lock also bumps model_epoch. Every discontinuity bumps model_epoch
before another mapping. No valid anchors are present in unlocked records.

Only one publisher may hold the object's exclusive advisory lock; it is
acquired before creating the audio tap, so a rejected second publisher cannot
reinitialize the first writer's audio object. Creation
failure warns and leaves playback running. Clean shutdown zeros and unlinks
its owned timing object; already mapped readers see state none. The audio
object follows the unchanged core sink lifecycle. An unclean exit leaves a
stale heartbeat, so readers must never accept state alone.

## Journal and owner reader

`yeney: timing` still appears every 10 s and at epoch end. All original fields
retain the **midpoint comparison model** meanings: edges/inliers, median full
window_ms, residual_p50/p95_ms, empirical uncertainty_ms (unknown before twenty
inliers), significance-gated drift_ppm, RTTs, encoder-acceptance lead p5/p50,
frame/audible_mono reference, rejected/errors, outliers/outlier_frac, span_s,
drift_sigma_ppm, missed_edges and adaptive half_window_ms. Missing ticks counted
by that comparison model include deliberately unprobed ticks after lock.
`mono`/`real` remain back-to-back MONOTONIC/REALTIME seconds, nine decimals.

Added fields: state, model_epoch, band_ms, violators, locked_drift_ppm (time
slope), locked_rate_drift_ppm (reciprocal audio rate), published_uncertainty_ms,
publish=on/off. New fields: one_sided_count (accepted upper/lower observations),
contradiction_count (observations outside the tolerated predicted band),
last_two_sided_age_s (age of the last usable adjacent bracket),
send_latency_p50_ms (rolling first-send lateness), and burst_span_ms (2H,
planned symmetric span). probe_rps remains the ten-second additional-request rate.
Final lines describe the ending model; reason explains its subsequent reset.

RAW still logs exactly one line per probe and every adjacent bracket, retaining
the original midpoint residual/classification for offline comparison:

```text
yeney: timing-raw room="Study" stream=3 epoch=7 seq=42 kind=burst send_mono=12345.210000000 recv_mono=12345.220000000 rtt_ms=10.000 reltime="0:00:12" reltime_s=12.000 outcome=accepted reason="sample"
yeney: timing-edge room="Study" stream=3 epoch=7 seq=42 second=11->12 lo_mono=12345.190000000 hi_mono=12345.220000000 mid_mono=12345.205000000 half_width_ms=15.000 residual_ms=2.000 classification=inlier rule=median-mad-width-floor
```

Outcomes include accepted, rejected-rtt, rejected-stale-epoch, error and timeout.
Send may be nan when no send occurred; receive is completion/deadline on error.
`seq` spans epochs. RAW off creates no request/edge lines. A low midpoint p95
can hide delayed counters; a large midpoint p95 can hide a precise intersection.
A locked band/uncertainty <=25 ms is an AirPlay-grade candidate; this field log
supports <=5 ms. Acoustic offset/asymmetric SOAP timing must still be calibrated.

## Owner procedure

On the deployment host (substitute room), preserve other overrides:

```sh
sudo systemctl edit 'yeney@<room>'
```

```ini
[Service]
Environment=YENEY_TIMING_PROBE=1
Environment=YENEY_TIMING_RAW=1
Environment=YENEY_TIMING_PUBLISH=1
Environment=YENEY_AUDIBLE_OFFSET_MS=0
```

```sh
sudo systemctl restart 'yeney@<room>'
scripts/yeney-timing-read 48:a6:b8:20:39:64
```

The Python 3 standard-library tool opens both objects read-only. Every second
it prints all decoded fields, audio abs_write_pos, predicted audible_mono_ns and
head_start_ms; null predictions mean unavailable/stale/invalid mapping. `--once`
prints a single snapshot. Use the room's actual MAC. Confirm coordinator and
our URI, then play 20 minutes without pause/seek/skip. Note start time and collect:

```sh
journalctl -u 'yeney@<room>' --since '2026-10-03 12:00:00' --no-pager -o short-precise > /tmp/yeney-timing-room.log
```

Replace the example time. Compare midpoint and bracket fields, lock continuity,
steady probe traffic, SHM generation/epoch and head start. For one-off acoustic
calibration, compare predicted audible time to measured speaker output and
set AUDIBLE_OFFSET_MS manually: positive makes predicted sound later, negative
earlier. No automatic offset or playback correction occurs. Afterwards remove
only these timing settings (or set PROBE/PUBLISH/RAW to 0), then restart.

## Decisions

1. Add a read-only inverse-clock query in Diagnostics; reuse the existing stream
   PCM base instead of reading SHM, so no timing publication or tap changes are needed.
2. Require finite uncertainty <=10 ms as the normal bound, leaving headroom above
   the observed 3.6–5.4 ms and the initial 8.5 ms lock limit; degrade to RelTime
   when age or drift uncertainty becomes larger.
3. Project the accepted RelTime interval forward by its monotonic age and model
   rate; repeated cached integers retain the first observation timestamp, so
   the one-second lease cannot cause repeated clamping onto whole seconds.
4. Reuse ConnectionPosition's last-audible high-water mark on both paths and
   keep its encoded handoff coordinate separate; the existing sink's handed-PCM
   cap and pause guard continue to govern LMS elapsed and track boundaries.
5. Include the existing publication calibration offset only when PUBLISH is on,
   matching that channel's anchor without changing its data or parsing behaviour.
6. Keep PROBE opt-in and PUBLISH optional; the default model-position setting
   changes reporting only when a qualified clock already exists. Preserve
   same-rate gapless continuity rather than reset a still-valid stream clock.
7. Sample CLI at 0.25 s using request/response midpoint timestamps and a centred
   least-squares fit after collection; skip overdue deadlines rather than burst,
   and print all samples plus maximum absolute residual and rate ppm. Default
   to 60 s and CLI port 9090, require >=0.5 s for two samples, and use a 2 s
   socket timeout so an unreachable server cannot block the owner indefinitely.
8. Test with virtual time and a fake CLI only, preserving all existing assertions;
   leave the owner's unrelated timing-read-study.txt untouched and untracked.
9. Point origin at the requested ThaYapeMan/sonos-lms repository, whose main
   matches the starting commit; the checkout initially pointed at YeneY. Use
   one main commit with no history rewrite, core submodule changes, live LMS
   queries, SSH access or deployment.

## Decisions

1. Retain the no-prior lock gates: A2 phase precision is ready at 10 s,
   40 brackets at 39 s, and the 60 s span is the last gate. The separate 120 s
   drift fit remains unchanged; shortening it would weaken rate evidence.
2. With a prior, replace the span/count wait with at least five precise brackets.
   A2 locks at 10 s instead of 60 s. Keep the 15 ms band, 2% violation budget,
   two-contradiction unlock and 10 ms LMS uncertainty limit unchanged.
3. Store only rate, sigma and wall-clock age under `/var/lib/yeney`, per hex UDN,
   because phase and monotonic timestamps cannot survive a restart. Use a
   versioned text record for inspection and atomic fsynced replacement.
4. Accept priors for seven days with absolute drift <=500 ppm and sigma <=15 ppm;
   these bounds allow the observed host/speaker rates while rejecting stale or
   implausible records. Never create storage during playback; warn once and continue.
5. Require 120 s of live rate evidence followed by 60 s of qualified lock before
   persistence. Estimate sigma from the feasible slope range / sqrt(12), floor
   0.1 ppm. Write changes >=1 ppm at most hourly, refresh age daily, and flush
   pending changes on clean shutdown to limit disk writes.
6. Invalidate a contradicted prior on disk as well as in memory, so a restart
   cannot repeatedly trust a known bad rate. Use live interval violations,
   the existing two-edge contradiction rule and a three-sigma live-rate check.
7. Seed acquisition position at integer RelTime +0.5 s: without phase evidence
   this minimises worst-case quantisation error to half a second. Use the actual
   SOAP midpoint and local monotonic age; leased copies do not become fresh observations.
8. Use a 15% reporting slew. A few-percent slew would take 13–22 s to correct
   the observed 650 ms error; 15% takes 4.34 s, without backwards movement.
9. Treat corrections above 750 ms as discontinuities, because slewing them
   would exceed five seconds. Clear slew state on explicit connection/stream
   resets; retain high-water, pause and handed-frame safeguards.
10. Use local producer journal lock events for field time-to-lock, matched by
    RINCON UDN/MAC. LMS CLI position alone cannot distinguish smooth acquisition
    from lock. Report unknown if evidence is unavailable, preserving current arguments.
11. Reference startup errors to the final stable linear fit and report both
    after-lock and after-five-second-slew statistics. The former includes the
    transient; neither is an acoustic truth measurement.
12. Reserve the requested yeney-core update for a separate commit so it can be
    reverted independently of startup position and drift persistence.
13. Install the missing clang-format 18.1.8 in `/tmp` for the core's required
    format check, keeping dependency setup outside repository and system files.
