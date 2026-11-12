# Navigation invariants

The room worker owns one arbiter for lazy PCM activation and speaker row
changes. `selections[index]` holds the occurrence token, producer-generation
lower bound, observed stream identity and phase: `Queued`, `Producing` or
`Published`. An already selected index binds with zero commands; another
request joins its existing owner; otherwise it immediately owns one command
in `selectionOrder`. Only `advanceSelections` writes that command.

LMS has one producer. The writer drains each live WAV declaration before
selecting the next index, so a burst cannot cancel earlier readers' producers.
Publication and audible confirmation are separate acknowledgements; either
may arrive first. Confirmation retires older published burst selections.
Disconnected readers release their pending ownership. No global lazy target
or activation token blocks another index from acquiring an owner.

Empty or short reads of the known playlist are rejected during navigation,
restart or window edits. Clear requires a fresh empty reread and no work in
flight. A standalone q remains explicit stop authority; a confirmed user stop
may cancel owned requests, but an internal timeout may not. The known count is
generation-scoped: a fresh matching nonempty URL set disjoint from the old
playlist establishes a complete replacement generation, not a truncated read.

Producer pairing uses the occurrence token and a generation lower bound;
a cancelled intervening admission cannot hide a fresh producer. A valid frame-zero same-source target is always paired, whether complete or
still producing. The bridge sink matches the occurrence token, source URL,
format and source frame count, retains its immutable WAV across a navigation
flush, and skips the fresh producer's already-stored prefix before appending
its remaining PCM. The new producer owns the audible clock; the URI and bytes
stay fixed. A different source or unusable audio cannot pair.

Re-production serves only a target that cannot supply the requested start.
Seek-partial history is converted to a deferred frame-zero route before the
outgoing navigation commits. Selecting that row holds HTTP headers until its
fresh producer is declared, then serves that producer directly at the selected
URI. It never serves the old seek suffix and never swaps an audible target.
LMS-originated navigation selects the declared row once; speaker-originated
navigation already selects its deferred row. Diagnostics report `paired` or
`re-produced reason=seek-partial|source-mismatch|incomplete`. The actual audible
URI anchors edits; a newer intent cannot be overwritten by the preceding
selection. Before removing a newly selected row, stop the plan and rebase from
owned Browse state. Retired lazy routes are cancelled only after commit.
The exact echo fence, URL shape, WAV promises and HTTP constants are unchanged.

## Seek invariants

I4: A seek never replaces a queue row whose immutable WAV holds the seek point.
  The renderer seeks inside that WAV; the new LMS producer attaches at its true
  output landing plus the reported settling bound, never at origin_ms.
  The producer discards that many first output frames and verifies 4096 frames
  against the retained from-start WAV before appending a suffix. A partial item is produced
  only when the landed frame is unknown or the produced prefix does not reach
  it, and the reason is logged.
I5: LMS keeps a seek start offset that STAT cannot rewind below. An
  app-originated position below that offset issues exactly one reason-checked
  `time` command; LMS's restarted producer attaches in-item and the renderer
  is never seeked or played again for that echo. The matching index-keyed
  Selection and speaker-rewind reason establish authority; position comparison
  does not. Every other app position change is zero-command.

## Decisions

1. Investigate parent c1359d4 before production edits. F1 dropped a different
   index at a global activation guard; F2 accepted one empty restart snapshot
   and cleared eight tracks; F3 appended a duplicate after rejecting a seek
   partial, faulted with `cannot replace window anchor`, then sent Stop.
   Preserve all three red tests and require them green on the fix.
2. Serialize publication as well as CLI writes. An immediate-write experiment
   cancelled earlier producers before their live readers received headers.
3. Keep both acknowledgements in the index record. Seed 1729 step 62 exposed
   publication before observation; seed 271828 step 103 exposed selection
   during an edit plan. Rebase rather than fault, and retain early observations
   of cached parents so a later Back is a new selection.
4. Confirm replacement generations explicitly. Compatibility checks exposed
   Stop followed immediately by a four-to-one playlist replacement. Empty
   reads and shortened subsets still cannot pass while navigation is pending.
5. Keep standalone q stop behavior and idle polling budgets. Metadata mode
   must not become an extra clear condition, and natural upcoming publication
   must not enable navigation fast polling.
6. Use loopback LMS, eight distinct own PCM tracks and the echoing speaker.
   Run three fixed 200-navigation-step seeds, with additional pause cycles.
   Keep the core/pin unchanged and the supplied consumer read-only; no owner
   endpoint, new runtime dependency or copied GPL/LGPL code is required.

7. NAV-ROBUST-02 pairs in the bridge sink before publication. A navigation flush
   retains a usable frame-zero prefix; the fresh producer skips that prefix and
   writes only its suffix. Source token, URL, format and decoded source count
   establish identity; stale LMS status time alone cannot veto that identity.
   Complete aliases keep their owned URL, including during cleanup.
8. Replace seek-partial history with a deferred frame-zero route while leaving
   that parent. A speaker Back binds its fresh producer directly to the selected
   route (zero renderer Seek); an LMS Back makes one renderer Seek to that route.
   This uses R2's direct-binding option and avoids a redundant Seek after audio
   is ready. The owner procedure must distinguish these two command paths.
9. Keep pending unproduced rows selectable during speaker bursts. Controller
   jumps still omit unproduced history. Use the existing superseded-selection
   check before moving away from a partial; never select an older burst intent.
10. Source identity is part of the producer's ordered storage admission.
    Register it on the same FIFO as flush and descriptors, and scope each flush
    to the producer generation captured when q arrived. Boundary callbacks can
    publish future facts before that job executes. A future local record was
    exposed to the preceding flush at seed 8675309 step 173; its
    `incomplete=flushed` then prevented publication forever. The unit test also
    posts a new boundary before the old flush runs. No extra clock/cancellation
    guard or timeout is needed. Natural-boundary tests wait
    for acknowledgement of their simulated outgoing position before switching.
11. Run all three 200-step seeds and the five new targeted cases on every
    `make test`. Each navigation records all selected URI changes, rejects a
    second audible target, and rejects re-production of a declared frame-zero
    same-source target. Private length fixtures and the read-only consumer are
    required for this round; no owner endpoint is used by the gate.
12. Keep lazy preparation independent of speaker switching. A selected deferred
    row and a verified retained target use producer-only admission for an active
    speaker selection. Check the arbiter's token and Producing phase, rather than
    a historical command-count entry, before taking that path. F1 asserts that
    GET-only preparation sends no renderer Seek.

13. SEEK-01b admits a pending boundary after contentPosition, then pairs a known
    landed frame only when the immutable prefix covers it. Producer coordinates
    skip the stored prefix, compare up to 4096 overlap frames and append the
    missing suffix. Range requests keep the original promise and byte identity.
14. A speaker rewind below the live producer offset owns one `time` command in
    the same index-keyed outbox as speaker navigation. The selection retains its
    target through producer admission; existing superseded-selection and echo
    fencing identify the echo and suppress every renderer Seek and Play, even
    when the playing renderer has advanced beyond the 500 ms tolerance.
    Non-rewinding app changes update STAT without a command. Backward speaker
    elapsed remains necessary for zero-offset app Back and rewinds above offset.
15. Starvation follows available immutable WAV audio, not only the restarted
    producer cursor. A forward app seek into retained PCM must not emit STMo
    while its paired producer catches up. Keep decoded-frame accounting bounded
    by submitted PCM; outputEmpty separately checks actual cached audio ahead of
    the reported speaker frame. Real exhaustion and cancellation still surface.
16. The seek path checks the arbiter's active selection phase and token, not a
    historical speaker command count. Seed 1729 step 101 starts an LMS seek just
    after a paired app Back, before the older slide diagnostic clears that count.
    Its stale count cannot turn the new seek into a TRACK_NR restart.
17. Each soak seed starts with seven contract steps: LMS seek 12, app 18,
    app 13 (above-offset rewind), app 5, app 0, app 18, app 0. The remaining
    193 steps are seeded random operations. Seed 271828's first random workload
    passed its behavior checks but had no above-positive-offset STAT case; keep
    the coverage assertion and guarantee that coverage rather than dropping it.
    Rewind echoes use the indexed command target as their authoritative offset;
    a transient LMS status can include the previous run's STAT elapsed.
18. Retiring an older producer releases its immutable WAV only after the last
    usable producer alias leaves the window. Seed 8675309 step 193 paired a new
    producer with an unfinished natural prefix, then retiring the earlier record
    cancelled its new audible HTTP response. Keep that shared asset alive; the
    waiting-reader unit also verifies release after its last owner is retired.
19. Compare parent-absolute positions. A partial/continued WAV's RelTime starts
    at its row origin; add origin_ms and the continuation frame offset before
    comparing with LMS's saved offset. The native podcast continue regression
    has origin 100 s and row position 8 s: it is at 108 s, so it must not send
    time 8. Exact full local WAVs keep origin zero and the R4 tests unchanged.
20. The forced-fallback B1/B2/F3 setup waits for Next's window commit and the
    deferred frame-zero history URI before issuing Back. The renderer announces
    its next URI before the fenced history replacement finishes; testing Back
    against a half-finished setup can observe the old row between remove/add.
    Keep all Back audio, command, switch and renderer-Seek assertions unchanged.
21. A complete cached GET can return while LMS opens natural decode-ahead.
    Synchronize fully-produced LMS-seek setup on a settled stream-opening
    handshake with decoded audio after reading the complete WAV. Retained
    producer aliases need not receive a fresh item-end, and occurrence numbers
    are not consecutive after random navigation. Require the latest opening
    to have audio and remain stable for 200 ms. One seed issued q only 4 ms after the
    natural s, received a wrong FLAC frame and BAD_HEADER from that opening
    connection. Use the existing admission handshake, without suppressing faults,
    relaxing assertions or changing the pinned core. S2/S3 still exercise seeks
    during ongoing production through the controlled download gate.

20. SEEK-02 uses output coordinates for converted landings only when the source
    landing is divisible by the decimation factor. The FIR radius divided by
    the factor, rounded up, bounds settling (128 at 48 kHz, 160 at 44.1 kHz).
    Unknown/nonaligned landings retain the existing explicit fallback. Prefix
    coverage includes the entire warm-up and the unchanged 4096-frame overlap.
21. A rewind echo with the same retained row and URI skips admission topology,
    admission Browse and no-op WindowEdits reads. A real edit still takes its
    ordinary ownership and UpdateID fences. Producer STAT coordinates begin at
    the landing, while overlap verification begins after settling; renderer
    REL_TIME remains the requested WAV position.

22. The real-time fake continues advancing during natural-boundary readiness
    waits. Finish the current paired producer, move within the prefetch lead
    (15 seconds remaining), and prepare the successor before moving to the
    outgoing track's final second; then acknowledge its clock and
    select the natural successor. Keep the existing deadlines and clock tolerance.

23. A fresh known landing cannot prove an unknown retained WAV origin. Follow
    pairedFrom to the original production before an interior attach; frame-zero
    aliases never upgrade that original coordinate. Unknown origin uses the
    existing landed-frame-unknown fallback, preserving immutable WAV promises.
