# Measuring the Sonos playback clock

Set `YENEY_TIMING_PROBE=1` for an owner-run measurement. Unset or `0` disables
it; other values warn once and fall back to off. This round does not label,
retime, delay or otherwise change audio, transport, position reporting or LMS.
No LXC is accessed by the development tools. The owner enables the setting and
collects the journal on the actual devices.

Only the selected room when it is the known group coordinator is probed. The
cached state must be exactly PLAYING, its URI must equal the URI assigned by
this bridge for the current stream, and LMS must neither be paused nor have
relinquished the speaker. A group member never probes the coordinator. Unknown
ownership/topology fails closed. Losing eligibility stops acquisition and ends
the current measurement segment; an in-flight reply is discarded. Ten seconds without a usable edge expires the model and reacquires, rather than
continuing to report old precision during a stall. A pause,
flush, seek/new stream, or new connection/PCM anchor also discards the model.
A gapless track boundary in the same stream does not.

A separate worker reads GetPositionInfo directly, outside the ordinary
one-second lease. It never updates the position cache, metadata, local address,
normal SOAP diagnostics or any playback consumer. Successful ordinary position
polls can seed the first predicted edge at the midpoint of the unknown
one-second phase. If no seed is available, initial acquisition samples about
every 340 ms. Acquisition can take tens of seconds; do not interpret its first
few edges as a calibrated clock.

For each additional request, CLOCK_MONOTONIC is captured immediately before the
first socket send and after the receive containing the full response. The sample
uses their midpoint. The previous sample showing n and the first showing n+1
bracket the edge; the interval includes both RTT half-widths. A three-point burst
around the predicted next edge contracts the search toward an 8 ms half-window,
expanding again when the prediction misses. Network RTT sets the achievable
minimum width. Lost replies can skip ticks without resetting a valid model.

Up to 120 recent edges are fitted with centered, inverse-window-variance weighted
linear regression and three robust residual rejection passes. Before ten edges,
clock rate is held at unity. Implausible fits beyond +/-2000 ppm are not accepted
as drift; the provisional fit holds unity. The model is measurement-only:

```
audible_stream_seconds = intercept + slope * (monotonic_seconds - origin)
audible_mono(F) = audible_mono_at_logged_frame
                + (F - logged_frame) / (rate * (1 + drift_ppm / 1000000))
```

F uses the existing stream PCM-frame coordinate and connection base from
ConnectionPosition. It is neither an LMS track-relative counter nor an offset
in compressed FLAC bytes. Initial discarded/reconnected PCM is handled by that
existing anchor logic. The frame/time pair printed in each line supplies a
reference for reconstruction; rate is printed in Hz. Gapless boundaries keep it.
The lead statistics record the first frame of each PCM batch at **encoder
acceptance**, against its modelled audible time. This is the encoder tap's
available lead, not proof that the same bytes have reached the Sonos socket or
its DAC. Frames accepted before a model exists are not assigned invented leads.

Requests have a separate 200 ms deadline and exponential consecutive-error backoff of
2/4/8/10 seconds. Repeated error details are counted rather than logged per reply.
The same rolling limiter survives stream resets and backoff: at most eight
issued requests in any one second and thirty in any ten seconds. These limits
apply to additional probe traffic; existing position polls are unchanged.

Every ten seconds while eligible, and once at the end of each measurement
segment, the journal prints one `yeney: timing` line:

- `room`, `stream`, `rate`, `epoch`: room and exact stream/anchor segment.
- `edges`: robust-fit inliers; `window_ms`: median full RTT-expanded bracket.
- `residual_p50_ms`, `residual_p95_ms`: absolute inlier edge residuals.
- `uncertainty_ms`: half the p95 bracket width plus p95 residual, a conservative
  empirical uncertainty estimate relative to the reported Sonos clock. It is
  not a statistically certified absolute acoustic confidence interval.
- `drift_ppm`: fitted stream-seconds per monotonic-second drift; `nan` before
  ten inliers. It does not command clock correction.
- `rtt_p50_ms`, `rtt_p95_ms`: up to 128 recent probe RTTs, including slow rejects.
- `lead_p5_ms`, `lead_p50_ms`: up to 4096 recent accepted PCM batches, in ms.
- `probe_rps`: additional requests in the last ten seconds divided by ten;
  `rejected`, `errors`: discarded samples/brackets and SOAP failures.
- `frame`, `audible_mono`: the fitted frame/time reference. `nan` means unknown.
- `mono`, `real`: CLOCK_MONOTONIC and CLOCK_REALTIME captured back to back in
  seconds, with nine fractional digits. Cross-container conversion also needs
  a disciplined wall clock; NTP offset and wall-clock steps add uncertainty.
- `final reason=...`: segment termination rather than a playback command.

A stable model with p95 residual **and estimated uncertainty <=25 ms** is a
candidate for AirPlay-grade timing; greater values or `nan` are not good enough.
Residuals alone can conceal a fixed offset or asymmetric network delay.
RelTime is a firmware playback counter: only an owner-run acoustic/loopback
reference measurement can verify its offset from actual physical output.
Neither a good fit nor the simulated truth test proves that hardware offset.
This round therefore exposes measured uncertainty honestly rather than claiming
an unobservable exact DAC play time.

## Decisions

1. Keep all playback, transport and LMS code paths intact. Add passive hooks at
   existing position reset/connection/first-PCM boundaries, pause/flush, and PCM
   encoder acceptance. All existing fixture assertions remain unchanged.
2. Use a dedicated worker only when the validated setting is exactly 1. No
   worker, shared diagnostic instance, extra SOAP, timestamp syscalls or timing
   output is created when off.
   Additional fields on StreamActivity carry an explicit measurement permission;
   its default is false for callers without that permission.
3. Probe only a known coordinator, exact PLAYING state, exact assigned current
   stream URI, and unpaused/unrelinquished LMS state. Recheck after SOAP and reject
   replies from old epochs. Join the worker before event/controller destruction
   and before process-exit cleanup; initialize shared diagnostics before it runs.
4. Bypass positionInfo and call the HTTP/SOAP read boundary directly. A null
   thread-local timestamp capture leaves all ordinary HTTP wire bytes and timing
   logic unchanged. Capture first-send and last-body-receive, not connect time;
   parse only RelTime and do not publish cache or metadata changes.
5. Bootstrap the acceptable RTT at 80 ms. After twelve observations use twice
   the room's rolling p95, clamped to 10..80 ms. This learns each room's observed
   distribution while rejecting responses that cannot resolve tens of ms.
   The synthetic 6..18 ms distribution with occasional 150 ms outliers exercises
   this rule. Hardware thresholds remain unvalidated until owner measurements.
6. Use a 200 ms absolute HTTP deadline; error backoff 2/4/8/10 seconds. Retain the
   hard sliding 8/1 s and 30/10 s limiter across every reset and error. No per-probe
   logging or extra retry messages. Budget exhaustion waits for capacity.
7. Seed acquisition from actual successful normal reads, never cache hits as new
   measurements. Search roughly every 340 ms until edges exist, then use a
   predicted three-point burst with adaptive 8..250 ms half-window. Include RTT
   in the bracket; missing seconds skip an edge instead of inventing one.
8. Fit at most 120 recent edges with inverse-width weighting (20 ms weighting
   floor), centered arithmetic and robust rejection. Weighting prevents broad
   acquisition brackets from biasing steady-state phase/drift. Expire stale models after ten seconds without an edge. Hold slope at one
   before ten edges or for implausible +/-2000 ppm fits. Print unavailable drift
   as nan; do not use the model to correct clocks or playback.
9. Reuse ConnectionPosition's canonical stream base and generation boundaries,
   with a separate diagnostic epoch to fence late replies and an explicit
   valid-anchor gate to reject late PCM handoffs after invalidation. Reset on pause,
   flush, new ID/seek and connection/first-PCM anchor; retain through ordinary
   PCM/gapless boundaries. Final lines describe measurement segments within a
   stream, since a stream can have multiple pause/reconnect segments.
10. Log encoder-acceptance lead, rather than guessing a frame index from compressed
    socket bytes. Keep 4096 lead samples and 128 RTTs. Later socket taps will need
    exact encoded-frame/byte attribution, particularly for Range retransmission;
    this round changes none of that behavior.
11. Print both residuals and a conservative bracket-plus-residual uncertainty;
    do not call residual alone an absolute play-time accuracy guarantee. Include
    frame/time/rate and monotonic/realtime pairs for later reconstruction. Unknown
    statistics remain nan. Cross-container clock discipline and firmware/acoustic
    offset require owner tests, not SSH or deployment by agents.
12. Nine deterministic 300-second simulations cover 0 and +/-200 ppm, three random
    RTT/jitter/loss seeds each, coarse normal-read seeding and slow outliers. Assert
    p95 prediction error <=25 ms after 30 edges and recovered drift within 40 ppm.
    Those bounds allow acquisition/noise but distinguish the imposed +/-200 ppm
    rate from zero. Independently verify every rolling request window.
13. Add local HTTP tests for actual uncached probes, unchanged leased position,
    periodic/final output, timestamps, error backoff and
    unset/off/invalid/paused/member/foreign/relinquished/wrong-stream gates. Add actual position/PCM anchor reset tests, stale-epoch
    rejection and gapless preservation. Root make test includes all new fixtures
    and the existing bundled core suite; pin the probe off for regression fixtures
    so ambient settings cannot add traffic. No existing fixture assertions change.
14. No submodule changes, history rewriting, SSH or deployment. Build and run all
    tests locally, commit and push normally; device verification remains the owner.
