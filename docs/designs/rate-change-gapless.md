# Continuous playback across sample-rate changes

Investigation at YeneY `4dd5d7f`, yeney-core `946047c`, 12 October 2026.
Design only: no playback, transport, codec, timing or HTTP implementation changed.
All source line references below refer to those revisions. No access to LXC 113
or a speaker was made. The proposed implementation needs owner device acceptance.

## Finding and field evidence

A decoded boundary is ahead of the audible boundary. Today the feeder switches
Sonos to a different HTTP URI as soon as the next track reaches it. That discards
the old speaker buffer and triggers another device startup. Correct LMS elapsed
reporting at `4dd5d7f` does not repair either audible defect.

Selected original lines are retained in
[`tests/fixtures/rate-change-listen.txt`](../../tests/fixtures/rate-change-listen.txt).
They come from PID 46308 in the owner's untracked
`field-2026-10-12-listen/study-listen-journal.txt`:

| Original line | Local time | Observation |
|---:|---|---|
| 482–484, 487 | 15:16:29.370–29.376 | Seek creates stream 4, 48 kHz; LMS identity says track 32671, time 575 |
| 509 | 15:16:30.121069 | Speaker panel first shows PLAYING |
| 691 | 15:16:43.334542 | `boundary frame=54072479 rate=44100 gapless=0` |
| 703–706 | 15:16:44.338157–44.338807 | Stream 5, 44.1 kHz; old encoder immediately reports stream mismatch |
| 707–708 | 15:16:44.340028 | Old HTTP response ends |
| 713–716 | 15:16:44.394152–44.394479 | New GET ACTIVE; fresh FLAC header |
| 717, 723 | 15:16:44.435806–44.466584 | New zero counter; panel TRANSITIONING |
| 735, 737–738 | 15:16:46.667803–46.672225 | PLAYING, fresh zero, STMs elapsed 4 ms |

The user's listening observation is approximately 14 seconds of the expected
16.09 seconds remaining on track 1, about 1.8 seconds discarded, then about
2.3 seconds silence. The journal corroborates the mechanism and ordering;
PLAYING and SOAP timestamps alone are not acoustic measurements. The supplied
15:16:44.43 TRANSITIONING approximation precedes the displayed 44.466584 panel;
use neither as an exact last-audible-sample timestamp.

Derived intervals: decoded-boundary to stream creation 1.003615 s; creation to
new GET 55.995 ms; creation to PLAYING 2.329646 s; panel TRANSITIONING to PLAYING
2.201219 s. Seek-tail PCM has `(54072479-53307551)/48000 = 15.936 s` duration,
slightly shorter than library duration minus seek (16.09 s), consistent with
frame-aligned seeking. With PLAYING as approximate onset, its end would be
15:16:46.057, about 1.719 s after the URI switch. Do not replace decoded length
with the rounded LMS duration when deciding whether old PCM is drained.

## Where the restart is forced

1. **Core:** `third_party/yeney-core/core/player.cpp:514` tentatively marks a
   queued successor gapless. `:732–745` clears that flag when source PCM rates
   differ, records `first=submitted`, and calls `sink.trackBoundary`. Submission
   occurs in `:804–890`, in up-to-256-frame batches; `submitted` counts accepted
   normalized PCM frames, not frames heard at the speaker. `:812` records the
   old track's final submitted frame. `:158–162` computes track elapsed using
   that track's rate and end cap. `:722–730` handles STMs separately.
2. **Sonos sink:** `core_sonos.cpp:46–61` stores the track rate and continuation
   state (from LMS commands), independently of the core gapless argument.
   `:63–86` packs 32-bit normalized stereo into 24-bit PCM, queues it, and exports
   the same frames to the optional tap. `:157–178` explicitly creates a new ID
   for `streamRate != b.rate`, even for a continuous queued successor. It
   publishes the new rate, resets the stream base and arms startup reporting.
   `core_sonos.h:24` bounds the feeder queue at 48,000 frames. Its roughly one
   second of staging explains the boundary/feeder separation, without proving
   an exact fixed one-second queue occupancy in this recording.
3. **URI/transport:** `yeney.cpp:102–111` increments the ID and resets position.
   `:505–512` makes the ID part of the HTTP URL. `:656–689` dispatches pending
   stream starts; `:633–653` calls `playStream` with that new URL.
   `upnp/own_speaker_control.cpp:180–201` performs `SetAVTransportURI`, then Play.
   It neither queues a next URI nor waits for the previous audio to drain.
4. **HTTP ownership:** `sbstreamer.cpp:54–60,89–105` saves a rate per generation
   and opens the ACTIVE request's encoder at that rate. Old generation GETs
   redirect (`:308–327`); new-generation activation cancels old ownership
   (`:440–458`). `SBEncoder::read` checks generation mismatch **before** draining
   pending encoded bytes (`sbencoder.cpp:230–249`). `sbstreamer.cpp:535–605`
   sends one FLAC entity as chunked HTTP, then the terminating chunk and closes.
   `upnp/http_server.cpp:56–115,230–288` parses and routes the request; the HTTP
   server does not itself decide sample rates or require a URI change.
5. **Encoder/header:** `sbencoder.cpp:119–153` initializes libFLAC once with
   channels, bit depth and sample rate. `open` rejects reopening an active
   encoder. `:188–215` encodes interleaved samples and accounts output frames;
   `:286–295` paces accepted PCM in milliseconds using this fixed rate. Each
   fresh ACTIVE GET begins with `fLaC`/STREAMINFO. Keeping the encoder but feeding
   44.1 kHz samples as 48 kHz would speed up playback and raise pitch by 8.84%.

This is a current implementation/device-compatibility policy, not a prohibition
in HTTP or an absolute prohibition in FLAC. FLAC frame headers can encode
changing rates, but decoders may stop on such changes, and STREAMINFO describes
only one format. [RFC 9639 §9 and Appendix C.8](https://www.rfc-editor.org/rfc/rfc9639.html#appendix-C.8)
recommend constant properties for compatibility. No evidence establishes Sonos
support for mixed-rate raw FLAC or concatenated native FLAC headers on this URI.

## Option A: convert PCM into one running stream rate

Use libsamplerate 0.2.2, `SRC_SINC_BEST_QUALITY`, stateful `src_process`. Its
[BSD-2-Clause COPYING](https://github.com/libsndfile/libsamplerate/blob/0.2.2/COPYING)
permits redistribution with notices; preserve the copyright/disclaimer in source
and binary distributions. This design introduces no GPL/LGPL resampler.
[The upstream FAQ](https://libsndfile.github.io/libsamplerate/faq.html#q4--if-i-ran-src_simple-on-small-chunks-would-that-sound-bad)
warns against independent `src_simple` calls on successive blocks. Use a
persistent converter and account both input consumed and output generated.

### Choose the session rate before the first output frame

| Policy | Benefits | Costs and failure modes |
|---|---|---|
| First track's rate, frozen until explicit stop/seek | First track and later matching tracks bypass conversion; least policy change for single tracks | 44.1-first sessions downsample 48 kHz tracks; playlist order affects sound and fixtures; lifecycle more complex |
| Fixed 48 kHz from session start | Predictable global frame clock; retains current maximum supported rate; 48 kHz bypass; no later upward rate change; deterministic tests | Every 44.1 kHz track is converted; 8.84% more frames and roughly that extra raw PCM throughput than native 44.1 |
| Highest seen, increase while playing | Avoids downsampling later higher-rate tracks | Increasing rate recreates this exact restart problem; cannot know future shuffled/radio tracks; highest-known in a scanned playlist is only safe if frozen before start |

Recommend fixed 48 kHz for modern mode, with exact-rate bypass. Legacy mode stays
on its existing 44.1 kHz path in the initial implementation. Session means a
continuous output generation; pause/reconnect retain its chosen output rate;
explicit stop/seek may start another generation. Do not increase the advertised
source maximum beyond the current 48 kHz as part of this change.

### Placement and coordinate contract

Place optional conversion in yeney-core **after decoder normalization and before
converted PCM enters `Track.output`**, ahead of transitions/gain and global
submission accounting (`player.cpp:680–698`). Preserve decoder source format
separately. Add an opt-in output-rate/converter configuration for the Sonos
client; other core clients default to the current bypass path. Prefer a bounded
worker/decoder adapter so sinc conversion cannot block the Slimproto event loop.
Configure this per client; no global format changes in generic decoders.

For this mode every `Track.output`, `produced`, `played`, `first`, `end`, skipped
count, transition window, sink batch, HTTP PCM coordinate and tap frame is in
**48 kHz output frames**. Decoder-consumed source frames remain separately
counted. Explicitly update/document `Sink`'s source-format wording
(`core/sink.h:21–24`) for the opt-in converted path; do not silently reinterpret
that public contract. `Track.format` used by output accounting must describe the
converted rate; source rate/bit depth must remain available for diagnostics.
`prepareOverlap` must not overwrite converted format with decoder source format
(`player.cpp:762` currently does so).

For a new 44.1 kHz track, `N` source frames represent `N/44100` seconds and yield
about `N*48000/44100` output frames. Elapsed is
`(audibleOutput-firstOutput-skippedOutput)/48000`, capped at the converted end.
It therefore stays in the new track's own seconds, with unchanged pitch and
speed. Dividing converted frames by 44100 would create an 8.84% elapsed error.
LMS's seek offset remains LMS-owned, as today; decoder-aligned seek duration is
not inferred from library metadata. Timed skips, output thresholds, fades,
crossfades, buffer fullness and end-of-track handling all use output units.

Use a rational duration accumulator rather than rounding each block. The
benchmark actually yields **480001** frames for 441000 input frames: blindly
appending everything returned at EOF is not a duration policy. Define cumulative
output length as the rounded cumulative ideal duration, carry the fraction
between tracks, and retain/discard converter edge samples to that exact budget;
error must stay below one output frame, including thousands of short tracks.
Drain converter state before setting `decoded` or the old end frame. Converter
startup lookahead and terminal filter response must not become inserted silence
or repeated padding. Reset on seek/flush/cancellation, not on every block.
For mixed-rate boundaries establish an explicit edge-extension/drain policy
with impulse and continuous-wave tests: filter support can produce a short edge
transient, and independently zero-extending adjoining tracks can click. Do not
claim sample-perfect continuous filtering across different input grids until
that policy is implemented and tested. Equal-rate runs retain exact bypass and
existing gapless behavior; no resampler should touch them unnecessarily.

Core gapless classification must compare **output** rates while preserving the
queued-continuation requirement. Same output rate keeps one encoder/GET/URI;
STMs/STMt retain the existing zero-to-1-ms gapless sentinel. Do not delete the
rate-change reporting fallback for legacy/native mode or actual reconnects.
Existing `position_state.h:63–146` and `sonos-position.cpp:62–80` use stream PCM
frames; their inverse model should see 48 kHz throughout. No counter reset,
phase acquisition or prior reload should occur solely because source rate
changes. The tap must export converted frames, before Sonos 24-bit packing,
so the affine 1:1 mapping in `docs/timing-probe.md:264–298` stays valid.
Conversion only in SBEncoder would break that mapping and core end/elapsed
accounting; a sink-only converter would instead require piecewise inverse
source/output mappings across every boundary, reconnect and skipped frame.

### Precision, buffers and fixtures

Convert normalized int32 stereo to float using 2^-31, then saturate/round back
to int32 before the current gain/transition path. Float has about 24 significant
bits; converted audio is not bit-identical to source even though FLAC remains
lossless for the resulting PCM. Preserve matching-rate integer bypass. Output
stays 24-bit modern/16-bit legacy; the SHM analysis tap stays its existing 16-bit
quantization. Sinc can overshoot ±1 even for bounded input: test full-scale peaks
and specify clipping/headroom before release. The following benchmark avoids
clipping with a −6.02 dBFS peak; it does not justify attenuation for all content.
Dither and headroom policy need owner review; do not silently lower bypass gain.

`sbencoder.cpp:28,97` retains **256 packet slots**, not 256 bytes or PCM frames.
Actual FLAC packet sizes vary with compression and content. Keep this capacity
and overwrite diagnostic; measure packet occupancy, bytes and duration under
converted noisy audio and concurrent rooms before changing it. The feeder's
48,000 frames become consistently one second at 48 kHz; core's 384,000 output
frames become eight seconds. Converted 24-bit stereo raw throughput is 288,000
bytes/s versus 264,600 at 44.1 kHz. Compressed network cost is not guaranteed to
scale by that ratio: interpolated low bits may compress less efficiently.
Bound converter queues and rate expansion before accepting input.

Preserve HTTP Server strings, chunking, session/stream URL syntax, ACTIVE/STANDBY,
Range history, metadata protocolInfo and transport order. Existing 44.1 kHz
encoder fixtures should remain native primitive tests. Add converted-mode
fixtures with 48 kHz STREAMINFO and one entity across 48→44.1→48; update only
fixtures whose configured output policy actually changes. HTTP byte headers
should remain byte-for-byte identical. PCM assertions for converted data need
numeric references/tolerances; bypass assertions stay exact. Canonical Range
must replay the original converted bytes, never re-run conversion from an
arbitrary byte position.

### Measured CPU and quality

Dev machine: WSL2 Linux 6.6.114.1, Intel i7-1260P, 16 logical CPUs visible;
GCC 15.2.0, CMake Release `-O3 -DNDEBUG`; libsamplerate tag 0.2.2 commit
`c96f5e3de9c4488f4e6c97f59f5245f22fda22f7`. NumPy 2.5.2; BLAS threads forced to one.
No system library was installed. Library build and benchmark files live in /tmp.

Three trials, ten seconds stereo per trial, 256-input-frame streaming blocks,
1024-frame output scratch; process CPU time includes ctypes, copies, allocation
and final concatenation, excludes sine generation and quality fitting. These
are conservative conversion-harness costs, not complete bridge costs or
measured LXC 113 performance. CPU percent means one core, not the whole host.

| Direction | Converter | CPU seconds/10 s (median) | One core | THD+N float, dB | 24-bit rounded, dB | 24-bit packed, dB |
|---|---|---:|---:|---:|---:|---:|
| 44.1→48 | best | 0.338792 | 3.388% | −140.077 | −137.073 | −137.156 |
| 48→44.1 | best | 0.337277 | 3.373% | −139.748 | −136.823 | −136.992 |
| 44.1→48 | medium | 0.111146 | 1.111% | −138.359 | −136.104 | −136.199 |
| 48→44.1 | medium | 0.102409 | 1.024% | −134.565 | −133.434 | −133.555 |
| 44.1→48 | fastest | 0.056784 | 0.568% | −118.981 | −118.944 | −118.948 |
| 48→44.1 | fastest | 0.058185 | 0.582% | −103.385 | −103.385 | −103.384 |

Quality input is a 997 Hz sine, −6.0206 dBFS peak, rounded to 24-bit and then
float32. Fit sine, cosine and DC at exact output frequency, discard one second
at each end; THD+N = 20 log10(RMS residual / RMS fitted fundamental), with no
weighting/bandwidth filter. Gain error was less than 0.000001 dB for best;
fastest was +0.000034/+0.000027 dB. This is one steady-state sine test, not a
worst-case SNR, alias rejection, acoustic or transient measurement.

Pinned upstream [`src/src_sinc.c:191–197`](https://github.com/libsndfile/libsamplerate/blob/0.2.2/src/src_sinc.c#L191-L197)
describes best/medium/fastest as 144/121/97 dB SNR and 96/90/80% bandwidth.
The public miscellaneous API page has older quality numbers; use pinned source
and measurements, not those stale numbers. Best is preferred because its wider
passband matters more than saving two percentage points of a dev core.

The table is the final run while the repository suite was running. An earlier
run measured best at 3.084/3.269%; this spread illustrates host-load variability.
The packed column models existing `pcm_pack.h:6–10` dropping low bits (signed
floor), with fitted DC removed; it is distinct from hypothetical nearest
24-bit rounding. It excludes gain/fades and a complete FLAC roundtrip.

For an LXC 113 class x86 deployment, budget approximately 3.4% of an equivalent
single core per continuously converted room, plus decoding/FLAC/network/timing.
Six rooms imply about 20% of one equivalent core for conversion alone. At a
hypothetical fourfold slower effective per-core speed, budget about 14% per
room (about 82% for six), before existing work. These are scaling estimates, not a
claim about the unknown target CPU, quotas, throttling or concurrent workload.
The owner should run this harness there and measure the whole service under
multi-room load before selecting a permanent default.

## Option B: drain the old stream before switching URI

Keep native PCM rates. At the feeder boundary stop admitting new-track PCM into
the old encoder; finish/flush old FLAC so its final buffered block reaches HTTP,
serve all old bytes, and retain its generation/URI/position until its end is
predicted audible. Only then allocate the new ID and issue SetAVTransportURI.
Prepare new encoding off-path if useful, but two served generations require an
explicit broker redesign: current generation checks and stale redirects forbid
prefetching an independent next URI while old audio continues.

Estimate `endMono = clock(oldEndOutput-oldConnectionBase, oldRate)` from a fresh
qualified old-stream mapping. Remaining lead is `endMono-now`, including PCM
already encoded but not yet audible. Add only a measured uncertainty margin
and transport dispatch allowance. Do **not** sleep a hard-coded 1.8 seconds or
add `lead_p50` to the decoded-boundary timestamp. `timing_probe_runtime.h:437–448`
computes lead from the first frame of an encoder-accepted batch and a midpoint
model; it is neither a direct speaker-buffer read nor the final-frame deadline.
The field reports lead p50 1553.546 ms at 43.334 and 1457.793 ms at 44.338;
those aggregates differ from the owner's approximately 1.8-second discarded
tail and are not interchangeable with it.

The old model is actually **locked immediately before the boundary**: line 689
has seven edges, a 12.788 ms band and 7.546 ms published uncertainty. At that
same timestamp the tap-boundary reset changes it to acquiring (line 690). By
44.338 only one 349.866 ms-wide bracket remains, with unknown published
uncertainty. B must snapshot the qualified old mapping **before** this reset
or defer that reset until drain; the existing feeder sees the weaker mapping.
The locked counter estimate could schedule a deadline to roughly its reported
uncertainty, but not certify acoustic drain. A prior constrains slope, not phase. Normal
RelTime has whole-second quantization and a one-second lease. Even a locked
counter bound does not certify DAC/acoustic time. Preserve the old mapping
until drain scheduling is complete, and cancel/rebase on pause, seek, ownership
loss or reconnect. Use bounded event-driven waits outside the transport lock;
current callbacks cannot block the core loop. Define a timeout/fallback for
missing model or unreachable speaker, exposing degraded native transitions.

If the switch is deferred approximately 1.7–1.8 seconds, the old tail should
survive, but the observed **roughly 2.3 seconds of new-stream startup silence
remains**. Switching earlier to hide startup throws away that tail again.
Waiting too long creates extra silence; waiting too little truncates PCM.
Finishing the HTTP response may make Sonos stop/reset RelTime before the
prediction, so cache the deadline first and test EOF behavior. Failing to flush
libFLAC's partial last block can itself underrun; waiting for bytes-sent or
`queue.empty()` is insufficient proof of audible drain. B is a native-rate
fallback, not a solution for seamless playback.

## Option C: device prefetch/queue or upstream normalization

**C1 — queue the next URI.** UPnP
[AVTransport:3 §§5.2.22, 5.4.3](https://upnp.org/specs/av/UPnP-av-AVTransport-v3-Service.pdf)
defines an optional next-resource action that can prefetch HTTP. This is a
protocol opportunity, not evidence this Sonos firmware supports gapless
mixed-rate FLAC radio. No `SetNextAVTransportURI`/`AddURIToQueue` implementation
exists here (`upnp/own_speaker_control.cpp:93,180–201`); the captured native
radio-style stream has unknown duration and empty NextTrackURI (original
journal line 421, not included in the small excerpt). Inspect the actual
speaker's advertised AVTransport SCPD and test supported actions first, by the
owner. A Sonos queue experiment would also change queue ownership/current URI,
metadata and external-source handling; it cannot be assumed equivalent to
calling SetNextAVTransportURI on this radio endpoint.

A usable experiment requires finite old-track EOF, separately addressable next
content, two simultaneously valid broker generations, next-URI prefetch without
stealing ACTIVE ownership, and track-aware RelTime/epoch handling. Current
`stream > current` rejection (`sbstreamer.cpp:309`) and `< current` redirect
prevent this. Device must demonstrate full old tail, prefetched startup, no
silence and correct counters across mixed rates and groups. Keep it a separate
owner-run protocol experiment; its success could offer native-rate continuity
but its implementation scope/risk is greater than conversion.

**C2 — fixed 48 kHz upstream from the beginning.** This is A's fixed-rate policy
with conversion moved to LMS. It could leave YeneY PCM accounting unchanged if
LMS actually delivers every track at 48 kHz. Current HELO only advertises a
maximum (`player.cpp:370`, `CoreSonosSink::maxSampleRate`, decoder maxRate); a
maximum of 48 kHz does not request conversion of 44.1 kHz. No verified
per-player forced-rate configuration was established in this investigation.
Validate installed LMS transcoder behavior, license and effects on other
players before relying on it. The requested permissive converter requirement
still applies to any newly shipped dependency; an external server policy is
not a reason to introduce a GPL/LGPL library into YeneY.

**C3 — mixed-rate FLAC in one response.** RFC 9639 permits some frame-property
changes but warns about decoder interoperability. Appending a second native
`fLaC` header is not the same thing as a supported chained container. No Sonos
acceptance evidence exists; this also requires frame/time mapping changes and
custom encoder handling. Reject as the production plan without a separate
owner device proof.

## Recommendation and estimated implementation scope

Implement A with fixed 48 kHz modern output from the first frame, best-quality
libsamplerate, matching-rate bypass and an opt-in rollout. It removes both the
premature URI switch and its startup silence while retaining the established
HTTP transport. It requires explicit output-coordinate accounting, not a
small encoder-only patch. Retain B/native restart behavior as fallback; explore
C1 separately only if retaining native rates is a priority.

| Option | Old tail | Remaining transition silence | Main cost | Confidence |
|---|---|---|---|---|
| A, frozen output rate | Preserved if converter drains correctly | No additional URI-startup gap expected | Conversion and core accounting | Strong software basis; physical acceptance pending |
| B, deferred native restart | Preserved within drain-estimate error | About 2.3 s observed startup, plus scheduling error | Drain scheduler/EOF work | Timing uncertainty after seek; cannot be seamless |
| C1, device queue/prefetch | Potentially preserved | Could be seamless if device prefetch works | Broker/queue/clock redesign | Speaker capability and mixed-rate behavior unproven |
| C2, upstream fixed output | Preserved if server always normalizes | Like A | Server transcoder policy/CPU | Installed LMS behavior unverified |
| C3, mixed-rate raw FLAC | Unknown | Unknown | Encoder and mapping changes | Interoperability risk; no device proof |

Rough engineering estimates, including implementation and tests, not promises:
A about 5–8 focused working days plus owner acceptance; B 2–4 days with a
remaining gap; C1 one day capability experiment, then 1–2 weeks if viable.

| File(s), proposed only | Change estimate and purpose |
|---|---|
| yeney-core `core/resampler.h/.cpp` (new) | 180–300 lines: bounded stateful adapter, rational counts, EOF/cancel, error handling and exact-rate bypass |
| yeney-core `core/player.h`, `core/sink.h` | 25–50 lines: optional fixed output policy and explicit source/output format contract |
| yeney-core `core/player.cpp` | 100–180 lines: source/output format separation, converter lifecycle, output-domain thresholds/skip/fades/end and overlap audit |
| yeney-core `Makefile`, dependency/license docs | 25–60 lines: optional BSD library detection/link and redistribution notices; core clients without conversion still build |
| `core_sonos.cpp/.h` | 20–50 lines: opt in modern output, boundary/tap consistency, keep native restart fallback |
| `yeney.cpp`, `audio_mode.h`, `Makefile`, installer dependency docs | 30–70 lines: rollout setting and validation, dependency packaging; no queue or transport change for A |
| `sbencoder.cpp`, `sbstreamer.cpp`, `upnp/http_server.cpp` | Expect no production changes for A; retain fixed-rate encoder and existing wire behavior |
| `position_state.h`, `sonos-position.cpp`, `timing_probe_runtime.h`, `timing_tap.h` | Expect no algorithm/ABI changes; audit fixed-rate invariants and add regression coverage |
| core converter/player tests, `tests/core_sonos_test.py`, encoder/streamer/timing tests | 300–500 lines: duration/quality/backpressure/EOF/clock/boundary and HTTP assertions |
| `docs/timing-probe.md`, `DEVICE-VERIFICATION.md`, README | 60–100 lines: output coordinates, dependency and acceptance procedure |

Publish intentional yeney-core commits first, then the parent pin, preserving
standalone-client defaults. No such implementation or dependency pin is part
of this design commit.

## Test and owner acceptance plan

1. Converter unit tests: both 44.1↔48 directions; mono normalized to stereo;
   uneven blocks and partial consumption; final drain; very short/empty tracks;
   rational count over at least 1,000 boundaries; pause/reset/cancel/backpressure;
   unity bypass exact int32 identity; errors/clipping, bounded memory.
2. Quality: reproduce the sine measurement below; add passband sweep,
   near-Nyquist alias rejection, impulses, silence/DC/full-scale and adjacent
   tracks cut from one continuous signal. Check discontinuities and edge support
   as well as THD+N. Test int32 normalization, gain/fades and final 24-bit packing.
3. Core/sink integration: 48→44.1→48 and reverse order; exactly one stream/GET,
   48 kHz STREAMINFO; source duration and output elapsed agree within one output
   frame; no loss/duplication at boundaries; old end caps remain. Preserve the
   same-rate 1 ms gapless STMs/STMt sentinel, all native fallback replays and
   pause/seek/reconnect/Range behavior. Check mixed-rate crossfade policy too.
4. Timing/tap: source-rate change leaves stream/model epoch and 1:1 tap mapping
   continuous; LOCKED stays locked if otherwise eligible. Converted SHM samples
   correspond to encoded PCM. Generation reset/export-drop fences still work.
   No backwards position; boundary window excess ≤100 ms beyond real time and
   flats ≤0.5 s except true content silence, with existing network jitter limits.
5. Local build and full `make test`, core checks and wire fixtures; performance
   with concurrent conversion/FLAC/timing loads. Keep 256 encoded packet slots
   and require no overwrite/underrun. There is no frontend to check.
6. Owner repeats full forced-cold/warm 720-second runs with tracks 32671 then
   32678 and existing checker, then removes diagnostic overrides. Separately
   load both, play, `time 575`, listen and record across the change. With A expect
   about 15.94 s of decoded tail (library remainder 16.09 s), **zero additional
   stream IDs/URI switches/TRANSITIONING events at the rate boundary**, no
   discarded ~1.8 s tail, and no added ~2.3 s silence. Measure acoustic gap with
   a recording/loopback and known contiguous test audio; target ≤20 ms added
   gap, no missing/duplicated samples in decoded-output tests. Natural silence
   within these two tracks must be distinguished from a transport gap.
7. Owner confirms target CPU/quotas and runs the same benchmark there, then
   measures whole-service CPU under all room services, packet occupancy and
   underruns for at least 20 minutes. No LXC deployment is needed for this doc.

Before implementation the owner should decide whether fixed 48 kHz conversion
is acceptable versus native-rate preservation, confirm legacy remains unchanged,
and agree on saturation/headroom/dither and the acoustic acceptance threshold.
Default recommendation is opt-in best-quality fixed 48 kHz until those checks
pass. Speaker models/firmware, coordinator/group behavior, EOF quirks, converter
edge transients and target CPU load remain open risks. This proposal does not
claim synchronization with other LMS players or eliminate intentional seeks'
startup delay.

## Reproducing the measurements

Commands run on the dev machine (a fresh /tmp destination is needed on rerun):

```sh
lscpu
cat /proc/version
gcc --version
python3 -c 'import numpy; print(numpy.__version__)'
git clone --depth 1 --branch 0.2.2 https://github.com/libsndfile/libsamplerate.git /tmp/yeney-src-022
cmake -S /tmp/yeney-src-022 -B /tmp/yeney-src-build \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON -DLIBSAMPLERATE_EXAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build /tmp/yeney-src-build -j 4
OPENBLAS_NUM_THREADS=1 python3 /tmp/yeney-rate-bench.py
```

Modern CMake initially rejected upstream's old minimum; the policy flag above
resolved it without editing source. Save the following harness as
`/tmp/yeney-rate-bench.py`. Output reports each CPU trial and the median, quality,
gain and produced frame count. NumPy is a measurement tool only, not a proposed
runtime dependency.

```python
import ctypes as C, time, numpy as np, platform
lib=C.CDLL('/tmp/yeney-src-build/src/libsamplerate.so')
P=C.POINTER(C.c_float)
class Data(C.Structure):
    _fields_=[('data_in',P),('data_out',P),('input_frames',C.c_long),('output_frames',C.c_long),('input_frames_used',C.c_long),('output_frames_gen',C.c_long),('end_of_input',C.c_int),('src_ratio',C.c_double)]
lib.src_new.argtypes=[C.c_int,C.c_int,C.POINTER(C.c_int)];lib.src_new.restype=C.c_void_p
lib.src_process.argtypes=[C.c_void_p,C.POINTER(Data)];lib.src_delete.argtypes=[C.c_void_p]
lib.src_get_version.restype=C.c_char_p
print(lib.src_get_version().decode(),platform.platform())
def convert(x,ri,ro,mode):
    err=C.c_int();s=lib.src_new(mode,2,C.byref(err));assert s and not err.value
    out=np.empty((1024,2),np.float32);parts=[];at=0
    try:
        while True:
            n=min(256,len(x)-at)
            d=Data(C.cast(x.ctypes.data+at*8,P),out.ctypes.data_as(P),n,len(out),0,0,int(at+n==len(x)),ro/ri)
            assert lib.src_process(s,C.byref(d))==0
            at+=d.input_frames_used
            if d.output_frames_gen: parts.append(out[:d.output_frames_gen].copy())
            if at==len(x) and not d.output_frames_gen:break
            assert d.input_frames_used or d.output_frames_gen
    finally:lib.src_delete(s)
    return np.concatenate(parts)
for ri,ro in [(44100,48000),(48000,44100)]:
    # Quantized 24-bit source, stereo, -6.0206 dBFS peak, 997 Hz.
    t=np.arange(ri*10)/ri
    mono=np.rint(.5*np.sin(2*np.pi*997*t)*2**23)/2**23
    x=np.repeat(mono.astype(np.float32)[:,None],2,axis=1)
    for mode in [0,1,2]:
        times=[]
        for repeat in range(3):
            begin=time.process_time();y=convert(x,ri,ro,mode);times.append(time.process_time()-begin)
        z=y[ro:-ro,0].astype(np.float64);tt=np.arange(ro,len(y)-ro)/ro
        a=np.column_stack((np.sin(2*np.pi*997*tt),np.cos(2*np.pi*997*tt),np.ones(len(tt))))
        coef=np.linalg.lstsq(a,z,rcond=None)[0];r=z-a@coef
        fundamental=np.hypot(coef[0],coef[1])/np.sqrt(2)
        thdn=20*np.log10(np.sqrt(np.mean(r*r))/fundamental)
        z24=np.rint(z*2**23)/2**23; coef24=np.linalg.lstsq(a,z24,rcond=None)[0];r24=z24-a@coef24
        db24=20*np.log10(np.sqrt(np.mean(r24*r24))/(np.hypot(*coef24[:2])/np.sqrt(2)))
        zpack=np.floor(z*2**23)/2**23; cp=np.linalg.lstsq(a,zpack,rcond=None)[0]; rp=zpack-a@cp
        packdb=20*np.log10(np.sqrt(np.mean(rp*rp))/(np.hypot(*cp[:2])/np.sqrt(2)))
        print(f'{ri}->{ro} mode={mode} frames={len(y)} CPU_s={times} median_core_pct={np.median(times)*10:.3f} THDN_dB={thdn:.3f} output24_dB={db24:.3f} pack24_dB={packdb:.3f} gain_dB={20*np.log10(np.hypot(*coef[:2])/.5):.6f}',flush=True)
```

## Decisions made in this investigation

1. Work through the lowercase checkout path because it is the authorized writable
   alias of the requested repository; verify starting HEAD before inspection.
2. Keep production and submodule contents unchanged because this round authorizes
   investigation/design only; commit the design and a small verbatim excerpt.
3. Preserve source line numbers in the design and original text in the excerpt
   so claims are reviewable without committing the large private capture.
4. Separate owner listening estimates, displayed transport state and counter
   evidence because journal PLAYING is not an acoustic timestamp.
5. Build pinned libsamplerate 0.2.2 under /tmp because it provides a reproducible
   BSD-2-Clause candidate without system or project dependency changes.
6. Use a CMake policy compatibility flag after the initial configuration failure
   because it resolves the old upstream minimum without source modification.
7. Benchmark both directions and three sinc qualities using 256-frame streaming
   blocks, three ten-second trials and process CPU time to match core batching
   and expose quality/cost choices; include harness overhead rather than hide it.
8. Use a quantized −6.02 dBFS 997 Hz sine, fitted fundamental and discarded edges
   to measure steady-state THD+N without clipping or FFT leakage; report 24-bit
   rounding separately and avoid generalizing one tone to worst-case quality.
9. Recommend fixed 48 kHz modern output with best-quality conversion and bypass
   because it avoids future rate restarts, preserves 48 kHz bandwidth and costs
   little locally; keep legacy native behavior and opt-in rollout initially.
10. Place conversion before core output accounting because encoder-only conversion
    would corrupt frame correspondence, elapsed and the SHM timing contract.
11. Specify cumulative rational output lengths after observing one extra EOF
    frame in the benchmark because per-block rounding/drain cannot define time.
12. Treat delayed switching as a fallback and next-URI support as an experiment
    because neither field timing nor a generic UPnP specification proves Sonos
    native-rate seamless playback.
13. Give qualified per-core scaling estimates for LXC-class hardware because its
    actual CPU/quotas were not inspected and SSH/deployment are prohibited.
14. Run the local make/build and full test suite even though the change is
    documentation-only, following the repository's verification instructions.
15. Use one parent-repository documentation commit, without core commits, because
    the only authorized deliverables are the design and quoted evidence.

16. Check truncation separately after reading `pcm_pack.h`, because the existing
    24-bit pack drops low bits rather than rounding to nearest; report both
    models and retain the CPU variation observed during the full test run.

17. Reuse the existing clang-format 18.1.8 under /tmp through PATH after the
    first full test invocation failed at core format-check with missing
    clang-format; do not install or change system/project dependencies.
