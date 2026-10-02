# Grouped coordinator HTTP recovery

The supplied 2 October 2026 Study/MBR trace establishes a GET/close/Range
recovery pattern on firmware 86.10-80260/ZPS12. It does not establish why the
initial connection closes or which response status that firmware accepts.
Previously every GET replaced the encoder and started a new FLAC header; a
byte offset from the previous HTTP entity consequently addressed different
bytes. This is a confirmed server defect, separate from the initial drop.

## Decisions

1. Keep a canonical encoder and entity-byte history for the current stream ID.
   Capture the actual libFLAC callback bytes, including the original `fLaC`
   header and metadata, before socket delivery. HTTP headers, chunk lengths,
   CRLFs, and terminal chunks are excluded from Range offsets. Do not decode
   or re-encode cached bytes. Use a mutex for bounded snapshots and copies.
2. Use a lazily allocated 32 MiB ring per encoder. It holds over 60 seconds of
   worst-case 48 kHz stereo 24-bit PCM-equivalent FLAC (~17.3 MB per minute),
   comfortably above the observed 0.6–1.0 MB offsets. Only the current
   canonical encoder is retained; superseded request workers release theirs
   when they finish. Old stream IDs redirect rather than using old history.
3. Retain an encoder after an uncommanded socket drop while LMS is playing.
   Socket lifetime and producer lifetime are distinct. `response_open` still
   requires an actual active HTTP request; retention must not hide legitimate
   device resumes from status polling. Explicit pause/stop, source relinquish,
   cancellation, and new stream IDs continue to end or replace it. Range
   recovery does not mint a stream ID or reset the audible position anchor.
4. Accept a single open-ended `bytes=N-` Range, with checked uint64 parsing;
   reject suffix, multiple, finite, malformed, and overflowing ranges via
   clean restart. Copy from N through the retained entity and then its live
   appends, without injecting a header. At N=0, the original header is naturally
   part of the stored entity. History-reader mode avoids filling an unused
   packet ring. It also finishes libFLAC's final partial frame at producer EOF.
5. Use HTTP 206, exact Content-Length, and `Content-Range: bytes N-M/*`.
   Sonos's primary streaming documentation requires 206 for partial audio and
   rejects services that send partial 200 responses. M is the last byte
   produced at response time, not an invented total length. The client can
   append the next finite response from M+1 as the same encoder produces live
   data; it must not receive extra bytes beyond M in this response. This is
   the deliberate tradeoff needed for valid HTTP: indefinite 206 cannot have
   a valid unknown last-byte position. Reject the earlier candidate of a 200
   indefinite continuation in favor of Sonos's explicit documented contract.
   New fixtures check exact response length, range syntax and continuous
   decoder/sample identity across successive responses. No repo capture proves
   ZPS12 acceptance of these growing-entity ranges; hardware verification remains
   with the user. Do not treat fixture success as a physical group test.
   References: https://docs.sonos.com/docs/streaming-basics and
   https://www.rfc-editor.org/rfc/rfc9110.html#section-14.4
6. At or ahead of the produced edge, wait up to 500 ms for the first byte.
   After headers, return exactly the available finite range. The next Range
   waits at its new edge; the producer keeps encoding while requests hand off. An evicted/unavailable/malformed offset gets 302 with no-store
   to the current URL plus `restart=<request nonce>`. That parameter explicitly ignores a
   forwarded Range header once and creates an ordinary fresh FLAC entity.
   Store the nonce with that entity: subsequent Range requests on the same
   restart URL resume normally, rather than restarting forever. This prevents
   the same failed offset from looping. Stale Range requests also use a restart nonce so a forwarded offset cannot address
   the next entity by accident. Non-Range stale-stream redirects retain their
   established current-ID URL. Pause/relinquished GET semantics take
   precedence over Range recovery.
7. Identify the observed recovery probe by a plain GET with Connection: close,
   an existing playable canonical history, and LMS playing. Give it 100 ms to
   reveal an immediate disconnect before changing ownership or encoder state.
   Range requests bypass GET-pair inference and never send LMS play. A plain
   request that survives the window follows the existing ACTIVE/STANDBY path,
   including genuine new encoder creation and held-GET/GET-pair semantics.
   The delay also covers a probe racing the original worker's cleanup.
8. One continuation line reports the requested offset and half-open history
   interval. Rate-limit unavailable-history diagnostics to one per five seconds;
   all failed requests still get an explicit restart response. Existing socket
   diagnostics remain for ordinary connections; Range disconnects do not
   masquerade as LMS/device transport changes.
9. Keep recovery in shared sbstreamer/sbencoder code for both engines. Preserve
   all old fixture assertions. Add history boundary/wrap/eviction/parser tests
   and real libFLAC replay fixtures for repeated probes and ranges, the produced
   edge, future offsets, stale IDs and changed PCM across a same-ID boundary.
   Execute the shared fixture with both YENEY_PLAYER settings, plus the existing
   real-binary engine A/B integration suite. These tests do not emulate a
   physical Sonos group or establish its undocumented status-code preference.
10. Restore event service/state/sequence diagnostics only for exact
    `YENEY_DEBUG_EVENTS=1`, read once at the first received event. Default off; test exact 1, absent/default, and invalid true in the existing
    GENA loopback fixtures, retaining all their earlier assertions.
    Subscriptions, callbacks, and transport logic are unchanged.
11. Do not change bit depth, pacing, socket buffers, or timeouts as a proposed
    fix for the initial drop. The cause is not yet measured on the affected
    track/group. Record the investigation below and recommend a controlled
    device capture rather than an unproven tuning change.
12. Work locally on main at 14bbb47, commit and push main after a full clean
    build, complete bridge tests and bundled core tests. No SSH or LXC deploy.
    Frontend tests are not applicable: YeneY has no frontend. Install any missing
    test-only formatter under /tmp, not in host packages. Keep the bundled
    suite's explicit skip if the optional LampaStream checkout is absent.

## Investigation of the first drop

No audio file for Mochakk – False Need (Extended Mix), original FLAC response,
packet capture, per-send stall durations, or deployed TCP socket statistics is
present. Track-selective drops and absence of Range in solo Port playback are
consistent with a group buffering issue, but do not prove one. The same affected track started on the ungrouped Port (firmware 97.1, core)
at 17:13:55 and had zero Range requests in the following ten minutes. This is a
stronger control than the different Rihanna track: it narrows the trigger to the
coordinator/group/firmware/network configuration rather than this track alone.
Firmware, speaker model and network conditions still differ between the grouped
Study coordinator and solo Port, so those factors cannot yet be separated.

Source inspection finds compression level 5, verify enabled, 1024-frame input
conversion chunks, and default libFLAC frame sizing. The pacing window is 250 ms
relative to elapsed monotonic time since the first read, checked before accepting
a whole PCM batch. It is **not** a measurement of coordinator/member buffer depth;
a batch can overshoot that lead. Squeezelite stages up to 2048 frames. Core stages
its decoded batches through a bounded feeder. The same encoder and HTTP path
serve both. Each HTTP chunk is at most 16384 entity bytes. Writes retry partial
socket sends; SO_SNDTIMEO is 500 ms per syscall, so partial progress can extend
a complete chunk write beyond that. YeneY does not configure SO_SNDBUF: Linux
socket buffer sizing and autotuning apply. A TCP send failure does not prove a
FLAC encoder error or reveal how many bytes the coordinator consumed.

A local synthetic 10-second signal (five seconds silence followed by seeded
full-scale random PCM) encoded with libFLAC CLI level 5 gives the following.
This tests a bitrate spike, not this track or deployed real-time encoding:
16-bit 44.1 kHz stereo, synthetic silence→noise at 5 s: file=884648 bytes, frames=108, peak frame=16394 bytes, encode=0.015 s for 10 s PCM
 peak frame rate at 4096 samples: 1412.1 kbit/s; 250 ms encode lead: 44100 uncompressed bytes
24-bit 44.1 kHz stereo, synthetic silence→noise at 5 s: file=1325910 bytes, frames=108, peak frame=24586 bytes, encode=0.019 s for 10 s PCM
 peak frame rate at 4096 samples: 2117.7 kbit/s; 250 ms encode lead: 66150 uncompressed bytes
Local test-kernel TCP SO_SNDBUF before connect: 16384 bytes (not the deployed LXC)

The measured default frame interval is 4096/44100 = 92.88 ms. Peak dense frames
are about 16.4 KB at 16-bit and 24.6 KB at 24-bit, with the latter split across
HTTP chunks. Twenty-four-bit output increases the incompressible bandwidth by
50%; it does not show an encoder CPU shortage in this synthetic run. Local
SO_SNDBUF before connection is not the actual connected socket's effective
buffer, nor a measurement of the LXC. Sonos's FLAC guidance caps frames at 32 KB; measured 24.6 KB dense frames at
4096 samples/24-bit/stereo are below that cap. Its same page also contains an
older 16-bit-only statement alongside explicit 24-bit-partner guidance; the
actual S2/Port control here supports 24-bit and does not justify a forced
16-bit downgrade. No seek table or final frame-size metadata can be provided
for an encoder whose future PCM and length are unknown. Reference:
https://docs.sonos.com/docs/flac-best-practices

Encoder ring capacity is 256 packets;
its overwrite warning is evidence to collect, not proof of this trace's cause.

Recommendation: first verify resumed entity bytes and the selected 206 response
on Study grouped with MBR. Capture one full affected track and one control track,
including the gapless boundary, with YENEY_DEBUG_EVENTS=1 and the existing GET
header diagnostics. Record TCP FIN/RST origin, bytes acknowledged, retransmits,
zero windows, frame sizes, producer wait times and socket send durations around
seconds 5–8. Compare the same group and network at the current 24-bit setting and
YENEY_AUDIO=legacy (16-bit/44.1 kHz), while accounting for legacy's different
transition behavior. If the coordinator is draining faster than the 250 ms
producer lead permits, measure its required prebuffer and then test a bounded
larger lead independently of the Range fix. If socket stalls reach 500 ms,
investigate the group link and receiver window before changing that timeout.
Do not lower bit depth or enlarge buffers solely because the retry loop existed.
