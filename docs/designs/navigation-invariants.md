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
a cancelled intervening admission cannot hide a fresh producer. Only exact,
complete frame-zero retained audio is reusable. Other targets flow through
`navigationItem` into the fenced fresh-parent replacement. The actual audible
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
