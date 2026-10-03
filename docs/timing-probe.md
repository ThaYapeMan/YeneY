# Sonos playback clock and LampaStream timing contract

Measurement and publication never correct playback, transport, LMS position,
clocks, or audio. CLOCK_MONOTONIC is the time base; YeneY and LampaStream must
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

Lock requires >=60 s span, >=40 brackets, band <=15 ms and <=2% violations.
All acquisition holds unity slope before 120 s. Once a room has locked a
120 s drift, that drift is retained in memory across epochs as the centre of
the first slope search when the new span reaches 120 s; offset is always
reacquired. No new drift is published before that span. Before lock published
uncertainty is unknown; after lock it is half the chosen band plus a 1 ms guard.
This is a counter-based bound, not a certified acoustic/DAC bound, and it does
not include the union of possible slopes or arbitrary future clock changes.

Locked probing targets one tick every LOCKED_EVERY seconds, with up to three
samples at prediction -12/+4/+20 ms, compensated by median RTT/2. Typically two
requests suffice: default steady-state traffic is about 0.4 requests/s. A single
bracket excluding the prediction by >2 ms is held as an unconfirmed mismatch;
it cannot move the published contract. Two consecutive mismatches (or two
failed locked bursts) clear the clock and return to acquisition. Acquisition
retains the previous adaptive search and midpoint fit. A true 10% population
of brackets excluding the physical clock cannot meet a 2% violation contract;
it must fail closed, rather than pretend that wide observations and invalid
brackets are equivalent.

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
publish=on/off. probe_rps remains the ten-second additional-request rate.
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

1. Preserve the old midpoint estimator, APIs, RAW classifications and all old
   periodic meanings as comparison data. Add separate bracket contract fields.
2. Use the last usable bracket for expiry, independently of the midpoint
   comparison fit, preserving valid wide constraints through gaps. Use fixed
   600 s/1200-edge history, endpoint-sweep maximum consensus, bounded
   10/1/0.1 ppm slope refinement and centre selection; disconnected solutions
   use the nearest consensus grid solution instead of an infeasible centre.
3. Report band conditional on the selected slope, matching the field fixture's
   band criterion; disclose that slope-family extrapolation is not included.
4. Hold a first mismatch outside the model, confirming with the next edge;
   reset on two misses as well as two contradictory brackets. Keep limiter and
   error backoff independent of resets. Locked default bursts use -12/+4/+20 ms.
5. Retain qualified room drift in memory as the search centre when the new
   span reaches 120 s; hold unity before then and never retain offset. Convert
   time-slope ppm to reciprocal rate ppb for ABI.
6. Use the probe context epoch as model_epoch, additionally bumping it for
   mapping changes and lost lock so in-flight old samples are rejected. Add
   a 128-byte layout with size, validity bounds and reserved space. Bounds
   prevent applying a new epoch to older PCM still in the audio ring. Unknown
   uncertainty is UINT32_MAX; all unlocked anchors are zero.
7. Instantiate the unchanged bundled core SHM sink only with publication; the
   baseline Sonos path lacked a tap. Compile its existing source in the root
   build because libyeneycore.a excludes it. No submodule or audio ABI changes.
8. Observe committed SHM deltas at producer acceptance, attach them to feeder
   batches, and map existing streamBase/ConnectionPosition coordinates. A lost
   export invalidates/reacquires instead of inventing continuity. Same-rate
   gapless and canonical Range recovery preserve the mapping.
9. Keep publisher failures nonfatal, enforce one writer using flock, and zero
   plus unlink on clean shutdown. Limit stable heartbeats to 1 Hz; the tool
   uses a conservative 3 s freshness check and two-channel sequence validation.
10. Parse extra settings only with PROBE and calibration only with PUBLISH.
    Off-mode compatibility means PROBE off; PROBE on necessarily receives the
    requested estimator/polling improvements even if PUBLISH is off.
11. Keep every original field edge line verbatim, including the preceding
    65-edge epoch; assert the owner's 1232-edge epoch separately. Preserve all
    original fixture assertions; only extend core-fixture link/include inputs
    to cover the root-built SHM sink.
12. Distinguish wide brackets containing truth from genuinely delayed physical
    ticks. Keep previous midpoint simulations unchanged and add bracket
    simulations plus fail-closed inconsistent-tick tests rather than relax the
    2% publication budget. New deterministic seeds are 17/31/89; locked-worker
    RTT seed is 18. Add actual POSIX lifecycle, real SHM sink and concurrent
    seqlock tests, plus owner-tool byte-for-byte read-only fixture checks.
13. Keep all work on main; use a separate follow-up commit to set the reader
    executable in Git on this mounted filesystem. No core changes, device access,
    deployment, clock/audio/LMS correction or SHM-content/ABI modifications.
