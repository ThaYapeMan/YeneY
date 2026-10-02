# Speaker source ownership

YeneY releases the speaker when another source takes over. The policy lives in
bridge transport and HTTP code, independent of the core decoding pipeline.

## Decisions

1. Use `speakerUriDescription` as the single session ownership predicate: the
   URI must carry this process's session token and a numeric stream ID. Keep
   its existing URI identity semantics, including Sonos URI wrappers. Prefer
   `transportInfo().uri` when `uriKnown`; otherwise call `currentUri` once
   before each ownership decision. A failed read grants no device resume.
2. Gate all device state observation, device resume, and GET-pair inference on
   ownership. Retain the existing resume lease, pause-to-play transition,
   GET-pair timing, and held-GET behavior for an owned, active session.
3. On the first foreign URI observed after our stream has started, atomically
   latch relinquished, reset device resume bookkeeping, mark LMS locally
   paused, end the HTTP response, and attempt LMS `pause 1` once. Do this even
   if LMS has already paused or stopped: `pause 1` is idempotent and prevents
   late device events from waking playback. Do not retry the CLI command on
   repeated events, including if its first attempt fails.
4. While relinquished, ignore PLAYING, TRANSITIONING, PAUSED_PLAYBACK, and
   STOPPED for device pause/resume relay. Suppress pending bridge transport,
   delayed stop, stream-start retries, and PlayStream calls. LMS `p`, `q`,
   heartbeats, HTTP GETs, and an automatically restored owned URI cannot
   reclaim. Pending intents cannot execute until an explicit LMS command
   replaces them; `s`/`u` also cancel a pending delayed stop.
5. Only LMS `strm s` or `strm u` clears relinquished. Reset resume bookkeeping
   on reclaim. `s` uses normal new-stream allocation and PlayStream;
   reclaiming `u` forces same-URL PlayStream even if a restored GET is open.
   Normal, non-reclaiming `u` still feeds held GETs. Mark reclaim pending
   until PlayStream succeeds or retry reconciliation proves the current
   stream is already playing, so the old foreign cached URI cannot cancel
   the explicitly requested reclaim during setup. Existing retry limits apply.
6. Restoring our URI by itself changes source identity but preserves
   relinquished. Serve a restored GET only when the LMS transport flag says
   playing. While paused, immediately send an empty HTTP 200 with
   `Content-Length: 0`, including a GET already waiting when takeover occurs.
   This deliberately offers no audio and no LMS play request, without the
   HTTP 503 failure/retry behavior of an ordinary paused resume timeout.
   Ordinary owned-session paused GET timeout behavior remains unchanged.
7. Source transitions are deduplicated under a mutex and log only `ours` or
   `other <scheme>`, never foreign URL credentials. Log relinquish and LMS
   reclaim once per transition. Remove the existing per-GENA-event transport
   diagnostic. Preserve the existing change-only `speaker URI:` diagnostic
   because the device-test script relies on its stream/session identity.
8. Extend production-function extraction harnesses for the ownership helper
   and shared state. Keep all existing assertions in device_resume_fixture,
   stop_pause_cases, retry_cases, and yeney_timeline_fixture. Supply an owned
   URI in transport-only fixture stubs; configure own_poll_fixture's mock
   speaker with this session's URI, rather than its previous external URI,
   because its original pause/resume expectations require an owned source.
9. Add Port-style repeated transport timelines for AirPlay, Spotify, line-in,
   and Sonos queues with the core player; verify cached URI use, one pause,
   no device play/PlayStream, automatic restoration, explicit s/u reclaim,
   and exactly one legitimate PAUSED-to-PLAYING resume. Extend HTTP tests
   for restored delivery while LMS plays and repeated empty 200s while paused.
10. Validate with a clean local build and the complete `make test` suite,
    including all C++ fixtures and `tests/player_engine_test.py`. Frontend
    tests are not applicable: YeneY has no frontend. Commit and push master;
    do not SSH to or deploy on any LXC. Device deployment and verification
    remain with the user.
11. The checkout and remote initially have only `main`, at the requested
    starting commit `1a8eb2b`. Create `master` from that commit and publish the
    fix there as explicitly requested; leave remote `main` unchanged.
12. Cache a successful SetAVTransportURI assignment before Play, using the
    state store's existing revision rule to preserve newer URI events received
    during SOAP. Otherwise a stale pre-start/foreign URI could falsely
    relinquish immediately after a successful LMS stream start or reclaim.
13. Include the bundled core engine's own `make test` suite as well as the
    bridge suite. Its required clang-format tool was absent; install version
    18.1.8 under `/tmp` solely for that check, without changing host packages
    or vendored engine source. The existing optional LampaStream integration
    test is skipped because that separate checkout is absent; retain and report
    the suite's explicit skip rather than fetching another project.
