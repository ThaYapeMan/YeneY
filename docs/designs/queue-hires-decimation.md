# Queue hi-res decimation: measured quality

Original symmetric Kaiser-windowed sinc, beta 18.5; unity DC gain. The cutoff is halfway between 20 kHz and output Nyquist. Only the retained phase is evaluated, with symmetric tap pairs and four independent double accumulators. No SoX/libsoxr source is incorporated.

| Source Hz | Target Hz | Converter | Ripple to 20k (±dB) | Alias 25k dBFS | Alias 30k dBFS | THD+N 1k dB | THD+N 19k dB | 24-bit quantisation noise dBFS | Full-scale step clipped frames |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 88200 | 44100 | YeneY | 0.000059 | -182.473148 | -300.000000 | -144.948848 | -144.984837 | -149.274911 | 42576 |
| 88200 | 44100 | SoX -v | 0.000053 | -300.000000 | -300.000000 | -144.936455 | -144.984837 | -149.239920 | 46202 |
| 96000 | 48000 | YeneY | 0.000054 | -300.000000 | -300.000000 | -149.443171 | -149.443170 | -149.243022 | 45013 |
| 96000 | 48000 | SoX -v | 0.000053 | -300.000000 | -300.000000 | -149.443171 | -149.443170 | -149.253430 | 44969 |
| 176400 | 44100 | YeneY | 0.000137 | -300.000000 | -300.000000 | -145.018770 | -144.976570 | -149.282718 | 42127 |
| 176400 | 44100 | SoX -v | 0.000112 | -300.000000 | -300.000000 | -145.018770 | -144.976570 | -149.287733 | 40998 |
| 192000 | 48000 | YeneY | 0.000105 | -300.000000 | -300.000000 | -147.033844 | -149.443170 | -149.275542 | 47501 |
| 192000 | 48000 | SoX -v | 0.000112 | -300.000000 | -300.000000 | -147.033844 | -149.443170 | -149.281285 | 47815 |

- 88200 sweep: output RMS YeneY -9.385, SoX -9.384 dBFS
- 88200 noise: output RMS YeneY -25.177, SoX -25.171 dBFS
- 88200: 641 taps, delay 3.628118 ms; unquantised FIR ripple 0.00000001 dB, stopband maximum -178.09 dB, observed speed up to 59.30x
- 96000 sweep: output RMS YeneY -9.360, SoX -9.336 dBFS
- 96000 noise: output RMS YeneY -25.335, SoX -25.155 dBFS
- 96000: 513 taps, delay 2.666667 ms; unquantised FIR ripple 0.00000001 dB, stopband maximum -179.94 dB, observed speed up to 69.17x
- 176400 sweep: output RMS YeneY -9.385, SoX -9.384 dBFS
- 176400 noise: output RMS YeneY -28.169, SoX -28.162 dBFS
- 176400: 1281 taps, delay 3.628118 ms; unquantised FIR ripple 0.00000001 dB, stopband maximum -178.07 dB, observed speed up to 30.55x
- 192000 sweep: output RMS YeneY -9.360, SoX -9.336 dBFS
- 192000 noise: output RMS YeneY -28.346, SoX -28.161 dBFS
- 192000: 1025 taps, delay 2.666667 ms; unquantised FIR ripple 0.00000001 dB, stopband maximum -180.33 dB, observed speed up to 33.26x

192→48 kHz stereo, one FIR thread, median of five 8 s runs including raw I/O: **29.98× real time** ([22.5143, 29.7117, 32.4391, 31.4364, 29.9812]).

Signals are generated as 24-bit PCM; tones are −1 dBFS. SoX uses `-D ... rate -v` with floating output, followed by the same deterministic 24-bit rounding for comparison. Tone fitting excludes 250 ms at either edge; no edge ringing is counted as harmonic distortion. Alias levels are fitted amplitudes; −300 dBFS denotes no measurable product. Noise floor is the white-noise requantisation residual; undithered digital silence stays exactly zero. The full-scale square/step deliberately exhibits intersample/Gibbs overshoot: YeneY guard frames and SoX diagnostic clipped samples/2 are reported, never hidden; normal tones/noise/sweep must not clip. Sweep and white noise are also converted and their RMS is reported above. Generated audio is temporary and is not committed.

Verdict: PASS

## Integration and accounting

The converter is enabled only by QueuePathSink, before ReplayGain, volume,
transition mixing and sink delivery. Ordinary CoreSonosSink/SHM clients keep
conversion disabled. Queue-configured remote flow uses the same converter.
The normalized 32-bit intermediate preserves eight guard bits; the existing
PCM packer rounds/clamps to 24-bit once. Default-off hooks leave native-rate
and ordinary stream paths unchanged.

Output count is ceil(source frames / integer factor), available with source
metadata. The centered FIR compensates group delay; new plays zero-pad the
leading boundary. Same-source-rate gapless successors inherit history and
phase, and supply the few milliseconds of future samples needed for the prior
tail. Each item owns its ceil count; with non-divisible counts ownership moves
slightly along the continuous output grid and extra allocation is confined to
the final padded tail. Odd-count unit fixtures compare all concatenated samples
against one continuous FIR, including exact per-item budgets.

A centered gapless filter requires successor lookahead: after raw EOF, STMs
and a durable produced prefix authorize one STMd even though the final FIR tail
awaits the successor. This exception is bounded to the filter tail; the route
waits for actual bytes. Missing successors flush after five seconds following
that request; short clips no longer than the filter radius and final items
flush directly. Unknown source counts still publish only after DSCO.
Headerless FLAC seeks use the existing exact-source-count callback, cached by
LMS track identity, with a bounded metadata-only Range fetch on a cache miss.
Seek origin remains the FLAC frame number, never an HTTP byte offset.

Reproduce with `make -C third_party/yeney-core decimator-fixture`, then
`python3 third_party/yeney-core/tests/decimator_quality.py --sox
/tmp/yeney-protocol-lms/Bin/x86_64-linux/sox --report /tmp/quality.md`
(on one command line). NumPy is a test dependency; the runtime has none.
The private LMS 9.1.1 SoX binary is test-only. No GPL/LGPL source is incorporated.
