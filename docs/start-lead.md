# Initial reserve and stream diagnostics

Grouped playback now starts each fresh FLAC HTTP entity with a bounded reserve.
`YENEY_START_LEAD_MS` defaults to **1000**. Valid values are decimal integers
0–10000, in milliseconds. An absent setting selects 1000; invalid values log a
configuration warning and select 1000. **0 selects the unchanged legacy 250 ms
pacing window**, rather than completely disabling pacing. A positive setting
replaces that allowance. The producer initially runs as fast as decoding,
encoding and socket delivery permit, then paces against the same monotonic clock
to preserve that lead. This is a target, not a guarantee that Sonos has buffered
that much: slow LMS/network/encoding or speaker backpressure can prevent it.

`YENEY_DEBUG_STREAM=1` enables compact per-response diagnostics. All other values,
including absent, leave diagnostics off. Example (values shown illustratively):

```text
stream 3: debug t=1.000 lead_ms=1972 bytes=787484 send_max_ms=0.082 rtt_us=89 rttvar_us=20 snd_cwnd=10 unacked=0 retransmits=0 total_retrans=0 end=open
stream 3: debug t=3.615 lead_ms=1958 bytes=147617 send_max_ms=0.080 rtt_us=108 rttvar_us=18 snd_cwnd=10 unacked=2 retransmits=0 total_retrans=0 end=our_close final
```

`t` is monotonic seconds since the first HTTP send. `lead_ms` is libFLAC's emitted
sample duration minus that response's elapsed wall time. It can be negative;
accepted PCM may be up to one FLAC frame ahead of emitted samples. For a Range
continuation the sample baseline is taken at response attachment: its line shows
new production since that attachment, not the duration of cached bytes replayed.
`bytes` counts actual socket wire bytes since the preceding line, including HTTP
headers/chunk framing and partial writes. `send_max_ms` is the longest complete
logical send call in that interval, including any partial syscall retries.
Linux TCP_INFO supplies microsecond RTT/RTT variation, segment-count congestion
window/unacked data, current retransmission count and cumulative retransmissions.
This does not measure Wi-Fi group-member buffers or delivery acknowledgments at
those members. Tests with synthetic transports report TCP data unavailable.

A line is emitted at each second through second 30, then at seconds 40, 50, etc.,
and one final line reports termination. Measurements happen on I/O/poll progress;
a blocking send can delay the line, and missed deadlines are not filled with
invented samples. `peer_FIN` means recv(MSG_PEEK) observed EOF before local close;
`RST/EPIPE` means reset/broken-pipe was observed on send or receive; `our_close`
means the server initiated shutdown without earlier evidence of peer termination.
FIN is the observed TCP read-side EOF, not proof of the speaker's application intent.

## Decisions

1. The supplied traces establish that a live, narrowly paced stream can stall
   in a group without a Range retry. They do not prove reserve starvation as
   the initiating cause. The wired Port leading wireless members also stalled,
   and an immediate replay succeeded, so Study firmware alone is insufficient
   to explain it. Keep this a controlled buffering experiment, not a claim that
   the dense opening or Wi-Fi hiccups have been measured on the actual track.
2. Choose 1000 ms by default: four times the previous allowance, with modest
   memory and reconnect exposure, while allowing trials at 3000 ms. It is not
   an experimentally established group requirement. Recommend measuring 0,
   1000 and 3000 rather than claiming a universal optimum.
3. Validate the entire setting as unsigned decimal, reject empty/signs/whitespace,
   overflow and >10000; accept zero and leading zeros. Parse once per module
   using pthread_once so encoder/output threads cannot race initialization.
   Configuration warnings can occur once in each module that uses the setting.
4. Positive lead uses the configured target, with the existing 250 ms branch
   retained exactly for zero. Gate before accepting a whole PCM producer batch;
   overshoot is at most that batch, including libFLAC's partial-frame storage.
   No artificial delay is inserted before headers or first audio delivery.
5. Put the pacing change in the shared encoder, so both engines use the same
   monotonic first-read clock. Every fresh encoder/entity gets a fresh clock:
   initial track, explicit q/s seek/new generation, pause=stop/u new response,
   genuine same-ID reconnect and Range-restart. Gapless PCM in the same encoder
   and ordinary byte-identical Range continuation retain the clock and lead.
6. Core already has an unpaced Sink, a bounded 48000-frame staging queue and a
   cancellable feeder; decoding can refill it while the feeder is gated at the
   encoder. Squeezelite already drains its decoded output buffer via the 2048-frame
   pump, blocking outside its output lock in the same encoder. Both can build the
   reserve without enlarging their PCM queues or adding a second pacing loop.
7. Keep the 32 MiB canonical history and 256-packet encoded ring. At supported
   44.1/48 kHz with default 4096-frame FLAC blocks, a 10-second reserve is about
   108/118 frames plus metadata and at most one producer batch. Dense 24-bit
   stereo frames are about 24.6 KB each. Concurrent HTTP draining reduces ring
   occupancy; a stalled reader remains subject to the existing overwrite policy
   and diagnostic. Do not enlarge the ring for a maximum well below 256 frames.
8. LMS elapsed/STMt continues to use Sonos's audible position, not accepted or
   submitted PCM and not wall-clock extrapolation. Core allows an initial STMs
   at the start of a new physical stream, then gates queued track boundaries on
   audible progress. This initial notification is required to establish the LMS
   session; it does not report the reserve as played time. Core completion waits
   for feeder drain and the audible coordinate when lead is positive.
9. For positive lead, squeezelite tracks separate submitted and audible track
   offsets in a bounded 64-entry boundary queue. The output lock protects it.
   Full queue applies backpressure rather than overwriting boundaries. Initial
   STMs starts the session; gapless STMs/epoch changes wait for Sonos to cross
   the boundary. Snapshot frame/device-buffer counts remain relative to the
   audible track. Underrun/completion require audible drain, preventing an empty
   local output buffer from falsely reporting a burst's buffered audio as ended.
   Generate three checked reporting hooks from the pinned upstream slimproto
   into an ignored build header; fail the build if those anchors change. Keep
   the squeezelite submodule source and revision unchanged. Preserve the
   DISABLE_SONOS_POSITION_FIX escape hatch by using legacy reporting when set.
10. STMd remains decode-complete in both engines, intentionally distinct from
    playback-complete: LMS needs it early to fetch gapless successor tracks.
    Do not defer it to the speaker or mislabel its earlier arrival as elapsed
    playback. STMo remains a true downstream-empty condition. Existing coarse
    UPnP clock, encoded reconnect PCM anchors, flush/seek and pause transport rules remain.
    For positive lead, defer applying a reconnect PCM offset to the audible
    clock until a fresh positive UPnP position is accepted: submitting the resume
    burst must not jump LMS elapsed while the speaker still reports zero;
    no synthetic playback clock is added to compensate for the reserve.
11. Preserve recovery probe, GET-pair, held-GET and foreign relinquish logic.
    Diagnostics decorate the response I/O without changing ownership decisions.
    TCP socket counters include partial writes; no TCP_INFO calls run when debug
    is off. Finish with one final line after the close, preserving earlier FIN
    or reset evidence instead of overwriting it with our shutdown.
12. Run all previous fixture assertions unchanged with explicit lead 0 in make
    test, to retain their historical timing baseline. Add separate dense 24-bit
    lead fixtures and real-binary engine A/B tests at 2000 ms. The Range fixture
    retains its assertions but closes its initial response sooner for a positive
    lead so its future-offset test still has live production ahead of it. Run it
    for both engines with lead 0 and 2000. This is a justified timing adjustment,
    not relaxed byte identity or HTTP assertions.
13. Verify real HTTP burst delivery, then steady pacing and one-batch bounds;
    actual Slimproto elapsed/STMt, delayed gapless STMs, no early STMu/STMo,
    pause/u and q/s fresh bursts, unchanged Play count across a gapless boundary,
    TCP_INFO fields, debug-default-off and validation edges. These loopback
    tests do not establish the physical group's required reserve.
14. Do not change bit depth, encoder frame sizing, TCP buffers, socket timeouts,
    Range response format or Wi-Fi configuration. Commit and push main after a
    clean build, complete bridge suite and bundled core suite. Keep the bundled
    suite's existing optional LampaStream skip when that checkout is absent.
    Frontend tests are not applicable: YeneY has no frontend. No SSH or LXC deploy.

## Device trial

The user deploys the commit. Use a group of three with at least one wireless
member, preferably wired Port plus Study and MBR. Keep group membership, track,
volume, bit depth and network conditions fixed. Enable YENEY_DEBUG_STREAM=1 and
YENEY_DEBUG_EVENTS=1. Run Mochakk – False Need (Extended Mix) from 0:00 several
independent times with lead 0, 1000 and 3000; include a gapless entry and Rihanna
as a control. Restart YeneY between settings because configuration is cached.

Record the lead, coordinator, firmware, membership, Ethernet/Wi-Fi link state,
start time, first disconnect/pause time and whether a Range retry occurs. Compare
first-second bytes and stable lead, send-duration spikes, RTT/variation, unacked,
retransmissions and FIN/reset/our-close at seconds 5–8 and 16–23. A successful
single replay is insufficient given the existing intermittent successful run.
If increasing the stable reserve reproducibly removes early stalls with otherwise
similar TCP metrics, that supports buffering starvation. If lead never reaches
its target, find the LMS/encoder/send bottleneck first. If stalls persist despite
stable reserve, capture coordinator/member traffic and receiver windows; the
instrumentation does not identify a wireless member's internal buffer state.
Keep pause/resume/seek and track/elapsed reporting in the same device trial.
