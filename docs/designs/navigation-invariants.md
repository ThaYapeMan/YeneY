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
