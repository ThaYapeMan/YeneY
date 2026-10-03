# Measuring the Sonos playback clock

This is measurement only: the model never corrects clocks, audio, transport,
LMS position reporting or the SHM tap. The time base remains CLOCK_MONOTONIC
on the same host as LampaStream. The owner runs all device measurements.

| Setting | Accepted values | Default |
|---|---|---|
| `YENEY_TIMING_PROBE` | `1` enables the probe; unset/`0` disables it | off |
| `YENEY_TIMING_RAW` | `1` enables request and edge records, only with the probe on | off |
| `YENEY_TIMING_STALE_S` | integer seconds, 10–600 | 60 |

Invalid values warn once and use the default. RAW and STALE are inspected only
when the probe is enabled: even invalid extra settings cannot add output when
PROBE is unset or `0`. There is no extra worker, SOAP traffic or timing output
when the probe is off. Ordinary playback and its leased position cache are
unchanged from the first timing-probe round.

## Acquisition and fitting

Only a known group coordinator in PLAYING with our exact current stream URI
and unpaused, unrelinquished LMS state is eligible. Members never probe.
Pause, flush, seek/new stream, reconnect/new PCM anchor and loss of eligibility
end the epoch. A gapless boundary preserves it. A fitted offset and rate survive
missing ticks; expiry occurs after STALE seconds without a usable edge. This
also bounds initial acquisition without any usable edge. Expiry emits a final
`reason=stale-edges` line and allows reacquisition. Coarse counter anomalies are
logged observations; existing transport/anchor hooks own discontinuity resets.

A separate worker calls GetPositionInfo outside the one-second lease without
publishing its result to the normal cache or its consumers. Successful uncached
ordinary reads can seed an initial prediction, but are never fitted edges or
additional probe requests. Initial search samples roughly every 340 ms until
five bracket candidates have been seen. Requests retain the 200 ms deadline
and consecutive-error backoff of 2/4/8/10 s.

Send and full-response timestamps bracket each SOAP observation; their midpoint
is the sample time. The last n and first n+1 response bracket a tick, including
both RTT half-widths. The midpoint of that interval is the edge observation.
RTT rejection remains 80 ms during the first twelve replies, then twice the
rolling p95, clamped to 10–80 ms. The 128 RTT samples include slow rejects.

The search half-window is at least
`clamp(3 * 1.4826 * MAD(inlier residuals) + RTT_p95/2, 20 ms, 250 ms)`.
It doubles after a missed prediction or skipped tick, capped at 250 ms, and
contracts by 75% after hits toward that floor. During acquisition, before twenty
precision brackets, spread is unavailable and the floor uses RTT alone.
Bursts start at the early search bound, then sample near the predicted tick and
20 ms afterwards. Subsequent ticks provide the adaptive refinement. A bounded,
deterministic phase shift (at most min(half-window, 40 ms)) prevents quantized
ticks from locking the sampling schedule to one side of the prediction. Half the
median RTT is subtracted from scheduled send time. Every observed transition
restarts the next burst, even if its bracket was rejected.

The original rolling limits remain eight issued requests per 1 s and thirty
per 10 s, across resets and errors. Reservations follow the captured first-send
time with a conservative 5 ms dispatch margin; this avoids boundary bunching
between budget reservation, socket send and the receiver's timestamp. These
limits cover additional probes; ordinary position polls remain unchanged.

Up to 300 recent bracket candidates (full width at most 650 ms) are retained.
Fit weights remain inverse squared width, with the existing 20 ms weighting
floor. Once twenty brackets of at most 100 ms are available, wider candidates
are excluded from the precision fit. This keeps broad recovery/acquisition
intervals from concealing a delayed tick. All bracketed transitions, including
wider ones, remain visible with RAW enabled.

Six centered regression/rejection passes and a final refit use residual median
and MAD, rejecting
`|residual - median| > max(3 * 1.4826 * MAD, edge half-width, 10 ms)`.
Classification uses the unconstrained candidate rate so a real drift does not
look like a phase outlier. The published model holds slope at unity until the
inlier span is at least 120 s, the estimated drift exceeds twice its standard
error, and that standard error is at most 15 ppm (a 30 ppm two-sigma precision
guard). While slope is held at unity, offset is recentered using the most
recent sixty seconds of inliers so unreported drift cannot accumulate as a
phase error. The full candidate history still estimates rate and its standard
error. The drift standard error includes a uniform-bracket variance floor to
avoid false confidence from unusually small residuals. Fits beyond ±2000 ppm
retain the existing unity-rate safeguard. Unknown drift and uncertainty before
twenty inliers are `nan`.

```
audible_stream_seconds = intercept + slope * (monotonic_seconds - origin)
audible_mono(F) = audible_mono_at_logged_frame
                + (F - logged_frame) / (rate * effective_slope)
```

F is the existing stream PCM-frame coordinate and connection base, not an LMS
track-relative position or FLAC byte offset. When `drift_ppm=nan`, effective_slope
is 1; otherwise it is `1 + drift_ppm/1000000`. The lead is measured at encoder
acceptance of the first frame in each PCM batch, using up to 4096 samples. It
measures the encoder tap's available lead, not socket arrival or physical DAC
output. Lead collection continues through permitted gaps without changing audio.

## Journal records

Every 10 s and at epoch end, `yeney: timing` retains the original fields:

- `room`, `stream`, `rate`, `epoch`: room, stream rate and measurement segment.
- `edges`: current fit inliers; `window_ms`: median full inlier bracket width.
- `residual_p50_ms`, `residual_p95_ms`: absolute inlier residuals against the
  published model, including the unity slope while drift is unavailable.
- `uncertainty_ms`: half the p95 bracket width plus p95 residual; `nan` before
  twenty inliers. This is empirical uncertainty relative to Sonos's counter.
  During gaps it describes retained edges, not proof of continued counter progress.
- `drift_ppm`: significant drift after a 120 s inlier span, otherwise `nan`.
- `rtt_p50_ms`, `rtt_p95_ms`: recent RTTs, including slow rejected replies.
- `lead_p5_ms`, `lead_p50_ms`: encoder-acceptance lead in milliseconds.
- `probe_rps`: last-ten-second additional request count divided by ten.
- `rejected`, `errors`: cumulative discarded observations/brackets and failures.
- `frame`, `audible_mono`: the model's frame/time reference, or `nan` if unknown.
- `mono`, `real`: back-to-back CLOCK_MONOTONIC/CLOCK_REALTIME seconds, nine
  decimals. Wall-clock discipline and wall-clock steps affect cross-host mapping.
- `final reason=...`: measurement termination; it never commands playback.

New fields are `inliers` (same as edges), `outliers` (excluded retained
candidates), `outlier_frac` (outliers / retained candidates), `span_s` (inlier
monotonic span), `drift_sigma_ppm` (candidate rate standard error), `missed_edges`
(cumulative skipped ticks/missed predictions) and `half_window_ms` (current
search half-window). Counts in the fit are rolling, not lifetime counts.

RAW adds exactly one record per additional probe request, including failures:

```text
yeney: timing-raw room="Study" stream=3 epoch=7 seq=42 kind=burst send_mono=12345.210000000 recv_mono=12345.220000000 rtt_ms=10.000 reltime="0:00:12" reltime_s=12.000 outcome=accepted reason="sample"
yeney: timing-edge room="Study" stream=3 epoch=7 seq=42 second=11->12 lo_mono=12345.190000000 hi_mono=12345.220000000 mid_mono=12345.205000000 half_width_ms=15.000 residual_ms=2.000 classification=inlier rule=median-mad-width-floor
```

`seq` increases across epochs; `kind` is seed (first probe), search or burst.
`reltime` is the received text, escaped for a single journal line; `reltime_s`
is its parsed seconds or `nan`. Outcomes are accepted, rejected-rtt,
rejected-stale-epoch, error or timeout. On failures, receive time is the
completion/deadline timestamp if there was no full response; send is `nan` if no
send occurred. The reason identifies RTT filtering, stale eligibility/epoch,
HTTP/SOAP/parse failure or a counter anomaly. An accepted request can still form
an outlier edge. Coarse normal-poll seeds are not extra requests and produce no
RAW request line.

Every bracketed transition gets an edge line. `lo_mono` and `hi_mono` include
RTT bounds. Signed `residual_ms` is midpoint minus the **pre-observation** model's
prediction, or `nan` before a model exists. Classification is the decision at
that instant; later refits may reclassify retained candidates. Rules identify
median/MAD acceptance/rejection or `wide-bracket`. A missing multi-second tick
cannot supply a bracket and is counted without inventing one. RAW off produces
neither request nor edge lines.

A stable p95 residual **and uncertainty ≤25 ms** is a candidate for AirPlay-grade
timing. Larger values or `nan` are not sufficient. A low residual can conceal
firmware counter delay, fixed offset or asymmetric RTT: only an owner-run
acoustic/loopback measurement can establish the offset from actual speaker
output. Raw delayed ticks must be investigated, not corrected by this probe.

## Owner measurement procedure

On the deployment host, edit the room (substitute its name):

```sh
sudo systemctl edit 'yeney@<room>'
```

Add these lines under `[Service]`, preserving other overrides:

```ini
[Service]
Environment=YENEY_TIMING_PROBE=1
Environment=YENEY_TIMING_RAW=1
```

Restart that room, note the start time, and play twenty minutes without pause,
seek or skip. Confirm it is the coordinator and owns the YeneY URI. Collect the
whole journal so resets and transport context accompany the raw observations:

```sh
sudo systemctl restart 'yeney@<room>'
journalctl -u 'yeney@<room>' --since '2026-10-03 12:00:00' --no-pager -o short-precise > /tmp/yeney-timing-room.log
```

Replace the example date/time with the noted start. Compare epoch continuity,
outlier fraction, missed edges, half-window, RTTs and raw tick delays before
judging residuals or drift. Afterwards edit the override again, remove only
these two measurement settings (or set both to `0`), and restart the room.
No development agent connects to or deploys on the device host.

## Decisions

1. Validate RAW and STALE only with PROBE enabled, preserving all off-mode
   traffic/output, including when the unused settings are invalid. Parse STALE
   as decimal digits only; leading zeroes are accepted, signs/whitespace are not.
2. Retain the model through gaps; use last **inlier** edge time for expiry and
   bound edge-free acquisition from epoch activation. Keep all existing anchor,
   transport and eligibility resets, and never reset from counter noise alone.
3. Retain 300 fit candidates, 128 RTTs and 4096 lead samples. The 600 s simulation
   asserts at least 300 lifetime accepted edges; rolling inliers necessarily
   remain below 300 when retained candidates include outliers.
4. Use six median/MAD passes, a half-width rejection allowance and 10 ms floor.
   Use a 100 ms precision-bracket cap after twenty narrow candidates, preventing
   wide recovery brackets from legitimizing 100–300 ms tick delays. Raw logs
   retain those observations and name the rejection rule.
5. Preserve inverse-width weighting and the 20 ms floor. Estimate candidate rate
   for classification but publish unity until the required span/significance.
   Add a ≤15 ppm standard-error guard: a merely significant 200 ppm drift
   with 30 ppm sigma is still too noisy for the requested 30 ppm reporting
   bound. Recenter the provisional unity-slope offset on the last 60 s of
   inliers; retain the full history for rate estimation, avoiding stale phase
   while a rate is not yet reportable. Standard error is centered weighted
   regression covariance with an empirical
   residual variance and uniform-bracket variance floor (1/12 in normalized
   weight units). Keep the existing ±2000 ppm physical plausibility safeguard.
6. Bootstrap five candidates with 340 ms search; acquire twenty narrow brackets
   before using residual spread to set the floor. Use 75% contraction, doubled
   missed-edge windows, RTT scheduling correction and a deterministic golden-ratio
   phase shift bounded by 40 ms. Probe the early bound, prediction and prediction
   +20 ms; rephase on every observed edge, including outliers. This avoids
   quantization feedback and stale burst phases without random wall-clock tests.
7. Keep the exact rolling budget and backoff across epochs/errors. Move the
   outstanding reservation to actual first-send time plus 5 ms, preventing
   small dispatch/receiver timestamp differences from bunching boundary requests.
8. Log only additional requests as RAW; ordinary read seeds stay passive. Quote
   room/text/reason, escape controls and preserve received RelTime text. Record
   completion time for failures and unknown send time as nan. Report each edge's
   signed pre-fit residual and decision, not a retrospectively cleaned trace.
9. Keep the original fields and empirical uncertainty formula; delay uncertainty
   until twenty inliers. Added counts describe the current fit candidate window,
   while rejected/errors/missed counts describe the epoch. Encoder lead and
   CLOCK_MONOTONIC/REALTIME pairs retain their original meaning.
10. Reuse seeds 7/19/73 for nine deterministic 600 s field simulations: 10% of
    ticks delayed uniformly 100–300 ms, RTT uniformly 4–20 ms, 1.5% lost replies
    (with the existing 2 s initial backoff) and 1.5% 60 ms RTTs; assert no stale expiry, ≥300
    accepted edges, ≤25 ms p95 truth error after 120 s and ≤30 ppm error whenever
    drift is reported. Preserve all original simulations and fixture assertions.
    Run the original ten-second expiry fixture with STALE explicitly set to 10
    so its unchanged assertion tests the former policy as a configurable case.
11. Add clock-injected settings/raw/gap/expiry tests without sleeps, plus the
    existing isolated real HTTP worker tests for cache, gating, rate limits and
    backoff. Add an actual HTTP RAW test matching every response text/timestamp
    and sequence to one request record. Root make test includes these and the untouched bundled core suite.
    Pin new settings off/default for ambient-independent regression tests.
12. Change only project-owned measurement code, tests and docs. No submodule,
    playback/reporting/SHM, history, branch, deployment or device-host changes.
