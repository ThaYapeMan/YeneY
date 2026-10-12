# Sonos playback clock and LampaStream timing contract

The locked measurement model now supplies continuous LMS-visible audible
position by default. It never changes playback, transport, clocks, audio,
the audio tap or timing publication. CLOCK_MONOTONIC is the time base; YeneY and LampaStream must
share the host kernel and make both SHM objects visible in the reader's
filesystem. The owner performs deployment and acoustic tests.
PROBE unset/0 preserves HTTP traffic and audio; the 1 ms gapless STMs sentinel
fix described below applies independently of PROBE.

| Setting | Accepted values | Default |
|---|---|---|
| `YENEY_TIMING_PROBE` | `1` enables coordinator measurements; unset/`0` disables | off |
| `YENEY_TIMING_PRIOR` | `0` forces cold acquisition without loading a disk prior; unset/`1` loads it | 1 |
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
without becoming fitted observations or additional requests. During early
acquisition (fewer than 20 midpoint inliers), a rejected edge returns to 340 ms
search sampling rather than predicting the next tick from the same fit. Three
consecutive rejected edges discard the transient midpoint fit, retaining RTT
history and the rolling request budget. Valid interval constraints survive this
scheduler recovery; brackets wider than 650 ms do not enter the acquiring
interval clock or refresh its staleness timer. Locked observations retain their
existing wide-bracket and one-sided semantics. No blanket startup wait is added,
so precise normal fixtures retain their original lock times.

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

The first accepted observation anchors at the middle of the one-second RelTime
interval (integer +0.5 s), using its actual normal SOAP midpoint when available.
Repeated leased values do not refresh their timestamp. Subsequent observations
constrain the unity/prior projection to their aged quantisation interval; they
do not attract it towards each new midpoint. If it leaves the interval,
re-anchor at the nearest endpoint. Before the first accepted observation the
existing anchor/start safeguards apply. Without phase evidence, expect roughly
500 ms initial phase uncertainty plus SOAP/lease uncertainty.

The locked model is inverted in the existing ConnectionPosition PCM coordinate,
including calibration if publication is enabled. It must match stream/base/rate,
be fresh under STALE and have finite uncertainty <=10 ms; active publication
must be locked. The model remains bounded to the aged latest RelTime interval.
Only qualified model convergence corrects at at most 15% of real time
(0.85–1.15x nominal): 650 ms takes 4.34 s and 750 ms takes 5 s. Qualified model
corrections above 750 ms jump as discontinuities.

The reporting coordinate retains fractional PCM frames in a double; only the
returned value is truncated. This avoids losing a fraction on every frequent
read. The main source of the field's slow acquisition was midpoint attraction:
one integer per 1.0045 s pulls the report towards -4500 ppm. When the polling
phase crosses a tick and skips an integer, that midpoint target jumps by about
one second and the old acquisition slew corrects it without a lock. Keeping
unity/prior projection inside the interval removes that attraction; an
unvalidated midpoint or interval fit never supplies the pre-lock rate.

An inconsistent delayed/repeated observation can require a bounded backward
re-anchor in the reporting state before lock. Both that observation bound and
unconditional monotonicity cannot be enforced for contradictory observations;
the normal field target remains no backwards samples. Encoded handoff and
sink safeguards remain unchanged. Explicit seek/new-stream/reconnect resets
discard reporting convergence state. Qualified model switches retain the
reporting high-water mark. `slew_offset_ms` is report minus target. Pause,
unlock, stale model, flush, new stream and reconnect retain the existing model
eligibility/fallback fences; same-rate gapless boundaries retain continuity.

The owner field check retains its arguments and original sample/regression
output:

```sh
scripts/yeney-lms-position-check <lms-host> 94:9f:3e:fa:ba:66 --seconds 600
```

It samples read-only `<mac> status - 1 tags:` every 0.25 s without catch-up
bursts. Time and playlist index/track ID come from the same response, avoiding
a track-change race between separate time and identity queries. A changed playlist index/track ID starts a new track group; without identities,
a position decrease starts a fallback segment. Each segment gets its own fit,
residuals, ppm, steps (including startup), longest flat interval, and before-lock,
after-lock and after-slew errors. Overall errors aggregate segment residuals;
overall ppm is the duration-weighted mean of segment slopes. No line is fitted
across a track boundary. Legitimate resets are excluded from backwards counts.
Within-track backwards anomalies remain in the track fit and are reported together.

It reads the local JSON journal, identifies the producer PID by its own `udn=` field containing the
MAC-derived RINCON UDN (rather than mentions in shared topology XML), then keeps that process's complete messages, including core STMs.
`time_to_lock_s` is lock MONOTONIC minus the initial STMs jiffies / 1000 for the
stream covering the capture, even if sampling starts later. Gapless STMs does
not create a new stream; following segments report `already_locked=1` and the
same stream lock duration. Missing stream/lock evidence is `unknown`.
Run on the YeneY host with journal read permission. The journal lookup covers
the preceding hour; longer-running streams need a separate complete journal.
Errors use each track's final stable linear fit, not acoustic ground truth.
After-lock includes convergence; after-slew starts at absolute lock + 5 s.
This also means a later track's boundary glitch remains visible in after-slew
errors. Keep playback continuous without pause, seek, skips or repeats.

For playing, unpaused network gapless tracks only, yeney-core changes
exactly-zero STMs/STMt elapsed to 1 ms. An immediate timer reply must not
replace the nonzero boundary sentinel with zero. LMS `Slim/Player/Squeezebox2.pm::songElapsedSeconds` returns
before interpolation when both elapsed fields are zero; 1 ms bypasses that
sentinel with at most 1 ms wire bias. Initial starts, non-network outputs,
paused reports, other event codes, audio, URLs, HTTP constants and transport
ordering are unchanged.
`Slim/Networking/Slimproto.pm::_stat_handler` replaces the stored play-point
fields on each STAT, and `getPlayPointData` returns that latest jiffies/ms/seconds
triple to Squeezebox2. Thus STMs zero remains authoritative until the next STAT.
Sources: https://github.com/LMS-Community/slimserver/blob/public/9.0/Slim/Player/Squeezebox2.pm
and https://github.com/LMS-Community/slimserver/blob/public/9.0/Slim/Networking/Slimproto.pm

The 6 October Study cold capture acquired at 325.213 s, with 325 interval
brackets, span 324 s, band 10.379 ms and zero violations: there was no lock gate
bypass or staleness reset. The scheduler's midpoint diagnostic count of 7–11
was a different estimator. The new scheduled counterfactual uses the first five
observed edge midpoints, later field-supported phase/rate, and captured RTTs;
it now locks at 61.222 s (62.227 s on 7a9213f). It is a simulation of the missing sub-second queries,
not a claim to have measured new device responses. Literal recorded rejection
replay verifies recovery deadlines and discarding transient midpoint edges.
Pre-lock replay stays within 500.000 ms of the latest quantisation midpoint
with or without the 10.850 ppm prior. The 20 s drifting-poll replay runs at -4280.045 ppm on start commit 7284fd9
and -1.134 ppm after the fix; with a prior it differs from that rate by less
than 1.5 ppm. A2 still locks at 60/10 s.

The segmented original cold checker capture shows 0.750 s flat at the second
track boundary and 854.250 ms excess step; warm has no flat interval. Warm
stream lock is 6.703 s. Its first segment after-slew max/p95 is 4.339/1.424 ms;
the second has max/p95 8.627/1.543 ms, including sampling/status jitter. Check
p95 as well as maximum rather than hiding isolated field outliers.

Measured Study A2 replay with normal SOAP timestamps: cold/prior locks 60/10 s,
pre-lock maximum errors 152.554/152.574 ms, after-lock maxima 114.800/115.033 ms,
after-lock p95 0.975/2.216 ms, and settled maxima 1.391/2.263 ms, relative to the
final fitted line. Without a SOAP timestamp the delayed-lease fixture's initial
maximum is 801.762 ms; extrapolation cannot reconstruct unknown read latency.
No backwards samples;
With known SOAP timestamps, internal 250 ms steps are <=287.5 ms. The
unknown-latency fixture crosses the existing >750 ms qualified-model
discontinuity threshold at lock (1051.875 ms step); this is a documented
exception, distinct from the fixed unqualified acquisition correction. LMS receives elapsed status once per
second and extrapolates between updates: at the checker, allow about 400 ms
per 250 ms sample during slew (150 ms excess), plus network/sampling jitter,
and about 250 ms when settled. For the physical Study,
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
9. Treat qualified model corrections above 750 ms as discontinuities, because slewing them
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

14. Leave core play-timing unset for Sonos and its analysis tap because no local
    pacer schedule exists. Verify the new extension flag/anchor/rate are unset,
    initialise recursive ALAC, and rebuild the library, standalone sinks, Sonos
    sink and test sinks against the new virtual interface. This preserves remote
    delivery timing while allowing the core's own paced clients to supply correct timing.

15. Always slew unlocked RelTime corrections, even when polling skips an integer.
    Such skips can be ordinary quantisation rather than seeks; retain immediate
    jumps for explicit stream resets and qualified model discontinuities.

16. Require a positive RelTime for smoothing after a new connection. The
    retained audible history must not let an old observation timestamp seed
    acquisition, including with start lead disabled; keep the original fallback.
17. Repeat the full suite after final position changes and without a concurrent
    standalone build, preserving the existing 500 ms socket-cleanup assertion
    rather than weakening it in response to a loaded-host failure.

18. Preserve acquisition request scheduling. A2 epoch 14 stays at a 65.512 ms
    phase band over 64 s; a rate prior cannot manufacture precise phase evidence.
    An experimental scheduling guide did not resolve a synthetic stalled seed
    and was removed. Only the evidence-count/span gate is shortened with a prior.
19. Distinguish internal slew bounds from CLI sampling bounds. LMS interpolates
    one-second elapsed updates, so the field target is about 400 ms per 250 ms
    sample during slew, not the internal 287.5 ms; allow measured network jitter.

## Core pin and optional play timing

YeneY pins yeney-core `45779a4` (formerly `88ea939`). Its additive `playTiming`,
`syncPause` and `syncSkip` callbacks remain source-compatible. The standalone
paced core sinks receive the core pacer's scheduled time, including accumulated
credit. YeneY's Sonos sink returns `paced() == false`: PCM acceptance, encoding
and HTTP handoff are not a correct playback schedule. Its private analysis tap
therefore never calls `playTiming`. The new audio timing block exists, but its
play-clock flag, play-time anchor and rate remain unset. The separate
`/yeney-timing-<mac>` Sonos bracket clock remains the audible timing contract.
The legacy audio layout and frame correspondence are preserved.

## October 11 acquisition and rate-change follow-up

The supplied October 11 journal is replayed separately from the October 6
fixtures. Only PID 43745 (cold) and 43848 (warm) are used. Acquisition starts
at stream 2 STMs, respectively MONOTONIC 1110013.688 and 1110750.554; stream 1
was LMS's earlier resume. The pause at 14:44:12 is the owner's stop.

A qualified prior now guides acquisition from the interval intersection.
Cold acquisition switches to the same guidance when the midpoint diagnostics
show a rejection or more than 30 ms residual, then retains that guidance while
the interval remains fully consistent. Precise existing acquisition fixtures
keep their original schedule. Guidance uses the interval's unity/prior slope,
its conditional phase band, and the existing golden-ratio sub-second dither.
It does not make the diagnostic midpoint fit authoritative. Scheduler recovery
still discards the transient midpoint fit after three rejections, but the edge
that triggered recovery also enters the independent interval clock. Genuine
interval contradictions retain the existing prior rejection and lock gates.
RTT limits, rolling request budgets and locked probing remain unchanged.

Across stream/PCM resets, a qualified live drift with sigma <=15 ppm is retained
as an in-memory prior even before it has met the additional disk persistence
age. Only stream phase and frame mapping are discarded. This never enables disk
prior loading when YENEY_TIMING_PRIOR=0; a forced cold process may still learn
and reuse its own speaker's live drift later in that process.

The core's queued-successor flag formerly remained gapless across a PCM-rate
change, although the Sonos sink created a new HTTP stream. The core now clears
that flag when the rate differs. For the restarted rate-change stream the sink
waits for a positive RelTime confirmation and audible position rather than
submitted PCM before STMs. Either the normal read or a current eligible probe
may provide that positive confirmation. An eligible probe zero may seed only
this armed internal coordinate without waiting for the normal lease; the probe does not update the leased
cache. This avoids waiting for the leased read that skipped the first integer.
A PLAYING zero alone may initialize the internal
reporting coordinate but cannot announce the new track. The 1 ms
sentinel remains for same-rate gapless successors. An explicitly armed zero
RelTime anchor allows the rate-change reporting coordinate to advance from the
first PLAYING zero-second observation, instead of waiting for a normal leased
positive integer and initializing at integer +0.5 seconds. Zero readings during
TRANSITIONING do not seed it; later stop zeros cannot reset positive position.
Arming survives repeated reset hooks for that same stream until its first
positive normal observation, and also ends on the next stream ID. Subsequent
reconnects retain the original positive-read safeguard. Initial acquisition's midpoint anchor remains unchanged.

Evidence: at 14:43:12.371359 the boundary logged rate=44100 gapless=1; at
14:43:13.374545 the sink created stream 3. STMs at 14:43:13.376495 reported 1 ms,
and STMt repeated it through 14:43:17.000. The first positive raw probe was
14:43:16.766410 (RelTime 1); the normal leased reporting path only initialized
later. STMt at 14:43:17.671059 jumped to 2558 ms: the positive integer,
0.5-second midpoint anchor and aged SOAP observation explain that coordinate,
rather than two and a half seconds of suddenly delivered audio.

New scheduler replays use the final interval-supported field phase/rate and
captured RTTs to answer counterfactual request times. They are simulations,
not newly observed speaker replies. Literal journal interval replay is also
reported. Cold gates still require a 60-second span between measured edges:
with the first edge after stream start, a strict <=60 seconds from STMs is
incompatible with keeping that gate. The field replay permits <=63 seconds
from STMs; warm and carried-drift phase acquisition require <=15 seconds.
The unchanged A2 evidence gates remain 60/10 seconds.

With track identities available, the checker keeps one fit per track; short
startup decreases do not split its reference line into micro-segments.
It prints a grouped anomaly's duration, maximum decrease and raw decrease
count. `track_change_gap_s` is the time between the two samples straddling the
identity change; `position_reset_s` reports the reset size. Acoustic silence
cannot be inferred from these captures, so `audible_gap_s=unknown` is explicit.
Per-track before-lock, after-lock and after-slew statistics include glitches.

Verified scheduler comparisons with identical simulated clock/RTT inputs:
October 11 cold 61.950 -> 61.931 s, warm with prior 23.851 -> 11.855 s.
The recorded producer locks remain 67.893/41.844 s from stream 2 STMs;
accepting all valid recorded interval evidence gives 61.400/41.830 s in literal
interval-only replay. The changed scheduling, rather than reinterpretation of
the original samples, supplies the warm improvement. October 6's existing
counterfactual improves 62.227 -> 61.222 s. The unsaved-live-drift rate-change
field-RTT replays reacquire in 14.256 s (cold capture) and 14.240 s (warm capture); the original cold field change took
36.253 s from its early STMs (37.258 s from tap boundary). A2 remains 60/10 s.


## October 12 non-gapless boundary correction

Core commit `946047c` supplies the end-frame cap and boundary jiffies.
The field run at c92b594 confirms cold/warm first-track lock at 61.9/7.9 s,
first-track after-lock p95 2.6/2.1 ms and second-track p95 1.3/1.1 ms.
The rate-change streams reacquired at 12.3/14.6 s after creation. Their remaining
error was reporting at the boundary, not slow clock acquisition.

`CoreSonosSink::feed` changed `streamBase` from zero to 28,371,728 before the
core's pending STMs. `audibleFrames` immediately evaluated the new connection's
startup position plus that base, even though the core still held track 1.
`Player::elapsed` divided that result by the old 48 kHz rate without bounding
it by track 1's `end`. The base jump alone reaches 591.077667 s; new startup
frames then masquerade as old-track overrun. Immediately before cold STMs,
`(593692 * 48 - 28371728) / 44100 = 2.84553 s`: the excess in the old 48 kHz
coordinate is the following STMs 2.846 s in the new 44.1 kHz coordinate, to
wire rounding. This identifies startup-coordinate leakage, rather than extra
old-track audio. The cold evidence is:

```text
12:31:12.367764 core: STMt jiffies=1189082835 elapsed_ms=587474
12:31:13.154210 core: boundary frame=28371728 rate=44100 gapless=0
12:31:13.368037 core: STMt jiffies=1189083835 elapsed_ms=588475
12:31:14.156906 Creating new stream (2) for Sonos
12:31:14.367538 core: STMt jiffies=1189084835 elapsed_ms=591145
12:31:17.369020 core: STMt jiffies=1189087836 elapsed_ms=593692
12:31:17.557866 core: STMs jiffies=1189088025 elapsed_ms=2846
12:31:18.369117 core: STMt jiffies=1189088837 elapsed_ms=2846
12:31:19.000521 core: STMt jiffies=1189089468 elapsed_ms=2846
12:31:19.369150 core: STMt jiffies=1189089837 elapsed_ms=2846
```

A current-URI zero was seeded just before TRANSITIONING at 12:31:14.315960
(epoch 22), then eligibility was lost at 12:31:14.348940. The global audible
high-water mark retained this speculative estimate through startup. Positive
counter evidence only permitted STMs at 12:31:17.557866. The later valid,
lower connection coordinate could not displace the retained high-water mark;
it froze at 2846 until real position caught up. Warm has the same sequence:
stream creation 12:44:47.590308, STMt 591139 at 12:44:47.791897,
STMs 2721 at 12:44:50.985708, repeated STMt 2721 at 12:44:51.794764.

The 2846 ms start is wrong. The cold 0->1 bracket has endpoints
1189087.672969442 and 1189088.025645743; its one-second tick implies zero
between 1189086.672969442 and 1189087.025645743. At STMs 1189088.025, elapsed
is about 1.00–1.35 s, not 2.846 s. A later precise edge (9->10 at
1189096.889046614–1189096.935422624) narrows the inferred zero to about
1189086.889–1189086.935, with negligible ten-second drift correction.
Thus audible-counter start was about 12:31:16.42–16.47, about 1.1 s before
STMs. The 3.401 s creation-to-STMs delay includes roughly 2.36 s of speaker
startup/TRANSITIONING plus waiting for the first positive integer. Counters
bound this phase; they cannot certify acoustic onset or silence.

The sink now snapshots the old reporting position before reset, extrapolates
it at its old rate, and caps it at the end frame until the restarted stream is
confirmed. The core independently caps each track's elapsed at its own known
PCM end. A stream-local zero can initialize/report only after an observed
eligible -> ineligible -> eligible cycle since the PCM anchor; a fresh positive
reply remains the fallback. This prevents a pre-TRANSITIONING zero from leaking
into either track. Unqualified normal zeros cannot initialize the coordinate.
A qualified zero advances immediately at unity/prior rate; STMs waits until its
position is positive, without adding or repeating a 1 ms sentinel. The existing
same-rate gapless 1 ms core rule is unchanged.

Positive-only startup anchors the conservative lower end of its quantisation
interval. Restarted-stream model convergence is limited to 5% (50 ms per
one-second STAT) to keep boundary-window excess below 100 ms; ordinary initial
acquisition retains its 15% policy. Same-rate gapless boundaries keep the current
reporting state without reinitializing it. Clock gates,
prior persistence, SOAP request schedule, PCM, transport and publication ABI
are unchanged. Pause, reconnect and stream-reset fences still apply.

The trimmed verbatim evidence lives in `tests/fixtures/oct12-boundary/`.
`oct12-boundary-test` runs the production sink with a supplied monotonic clock,
real ConnectionPosition/Diagnostics and captured response times/counters.
Encoder throughput and eligibility transitions are deterministic inputs.
Normal leased counters come from the status panel, using its display timestamp
as an approximation because their exact SOAP timestamps are absent. Raw probe
response timestamps remain literal. It
replays recorded STMt schedules, emits the new STMs when the production sink
permits it, and samples LMS's unity interpolation every 250 ms. The late old
STMs is not injected again. The window extends to ten seconds after the
original (later) STMs, so it also covers ten seconds after the corrected start.

| Boundary metric | Recorded cold | Replayed cold | Recorded warm | Replayed warm |
|---|---:|---:|---:|---:|
| Old STAT overrun beyond PCM end, ms | 2614.333 | 0 | 2500.333 | 0 |
| Largest sampled step excess, ms | 1597.533 | 0.000 | 1582.509 | 0.000 |
| Longest interval between equal samples, s | 3.000 | 0.250 | 3.000 | 0.250 |
| Within-track backwards samples | 3 | 0 | 2 | 0 |
| Largest backwards sample, ms | 561.967 | 0 | 622.068 | 0 |

The field's quoted 3.25 s freeze includes the first interval into the clamped
plateau; the checker measures the time *between equal-position samples* (3.00 s).
Likewise an individual warm STAT correction can approach 872 ms, while the
250 ms CLI sample decrease is 622 ms after interpolation. The original evidence
is preserved; no replacement measurements are claimed. Replayed STMs elapsed
is 6 ms cold / 3 ms warm at the two-millisecond event-loop resolution, then
advances. Listening and fresh owner captures must validate the device result.

New boundary logs append MONOTONIC jiffies. `boundary_window` reports old PCM
end/STAT overrun, total/longest CLI flat, largest excess step, backwards count/size,
and explicitly unknown acoustic gap. Legacy logs use the adjacent timing mono
and label that approximation. The old end uses the preceding core boundary's
rate and first frame, rather than LMS's rounded library duration.

Decisions for this follow-up:

1. Fix reporting only; the evidence identifies coordinate contamination, so do
   not alter encoder, HTTP, transport order, lock gates or probe scheduling.
2. Add a core end-frame cap and publish its commit before the parent pin; a sink
   startup coordinate must never be interpreted as completed old-track PCM.
3. Keep the old reporting clock separate until restarted playback confirmation;
   capping alone would retain a multi-second jump to the end.
4. Permit a fresh zero after the eligibility-loss/recovery cycle, with positive
   fallback; waiting for RelTime 1 unnecessarily delays STMs by another second.
5. Reject normal startup zeros until explicitly seeded by eligible probe
   evidence, preventing the normal lease from importing TRANSITIONING time.
6. Seed positive-only starts at the lower interval endpoint; an integer +0.5 s
   midpoint can start ahead and require backwards LMS corrections.
7. Limit restarted-stream convergence to 5%, retaining 15% elsewhere, because
   one-second STAT interpolation amplifies correction steps at CLI samples.
8. Add an optional sink clock callback for deterministic production-sink replay;
   the default remains the existing monotonic clock.
9. Commit only trimmed verbatim boundary fixtures, leaving all original and
   unrelated untracked evidence untouched.
10. Treat original captures as before-results and replay changed reporting with
    fixed responses as after-results; do not invent missing physical responses.
11. Add boundary jiffies and retain a labelled adjacent-timing fallback for old
    journals; acoustic gap remains unknown until the owner listens.
12. Install clang-format 18.1.8 only under /tmp for the existing required format
    check, keeping the system environment unchanged.
13. Use the existing case-insensitive checkout through its lowercase sandbox
    path for writes: both names resolve to the requested c92b594 checkout, but
    the sandbox denies writes through the uppercase alias.
14. Retain the existing deployment procedure and add the requested listening
    check; no agent accesses or deploys to LXC 113.

15. Ignore zero/rewound frame boundaries in the checker window detector rather
    than invent a negative old-track duration on explicit resets.
16. Use the captured carried rate with a conservative synthetic 7 ppm prior
    sigma in boundary replay; the prior's exact unpublished sigma is not field
    evidence. Existing acquisition fixtures continue to verify lock gates.

17. Freeze/rebase the retained old clock across pause/resume and clear it on
    flush; add a deterministic handoff-pause test so paused time cannot jump
    the old track to its end.
18. Commit the core from its existing detached submodule checkout and push
    HEAD to main, preserving local branch pointers and all published ancestry.

19. Replay the journal's displayed normal leased counters as well as raw probe
    replies, labelling display timestamps as approximations rather than claiming
    unlogged SOAP times were measured.
