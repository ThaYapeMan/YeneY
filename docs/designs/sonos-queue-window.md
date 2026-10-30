# Sonos queue window implementation record

YENEY-APPCONTROL-03 is incomplete. The room relay uses the fenced editor for
natural history trimming and protects two history parents and their audio.
Lazy playlist publication and LMS navigation reconciliation are still absent.
Next, Previous, restart and seek are not newly enabled. The README feature row
and step 3 qualification stay unchanged until controller integration and its
acceptance scenarios pass.

## Decisions

- `QueueWindow` owns parent occurrence tokens, one-based ordinal to zero-based
  LMS index mapping, and surgical edit planning. Constants are two history
  slots and three upcoming slots; the planned controller settle is 250 ms.
  Segment expansion maps actual Sonos ordinals back to the same parent LMS
  index and carries parent-relative frame offsets. Whether a preceding slot
  has retained audio or needs lazy production is a
  controller concern. This model alone does not retain played audio.
- The LMS status playlist order is authoritative. The existing status query
  also retains playlist timestamp, shuffle and repeat values without extra CLI
  traffic, rejecting invalid policy values. Identity is track ID plus
  source URL; title/artist/album changes preserve identity. Repeated identities
  match in occurrence order. This is deterministic but cannot distinguish
  deletion of one of two identical adjacent LMS entries from a status snapshot.
- A longest common subsequence anchored on the audible parent minimizes edits
  while keeping its queue URI. A missing anchor needs explicit playback
  replacement. There is no implicit full rebuild of a playing queue.
- `WindowEdits` treats all continuation URIs as one parent block. Every removal
  carries the freshly browsed UpdateID. AddURIToQueue has no UpdateID input;
  each single enqueue has fresh before/after Browse and exact URI/revision
  verification. A foreign edit, unchanged revision, or uncertain SOAP reply
  aborts the edit sequence without retries. The caller must resolve ownership.
- Lazy HTTP uses an immutable declared item and a separate produced item.
  Headers precede resolution; HEAD, WAV-header-only probes and invalid ranges
  do not request a producer.
  Before serving the body, room, rate, length and promise kind must agree.
  Resolver failures disconnect after the original response headers.
- `LazyItem` queues activation once across concurrent GET/range readers, waits
  with a 15-second default deadline, checks peer close at most 50 ms apart, and
  fences cancelled/late producers. A naturally prepared item can bind before
  activation and therefore requires no selection request. The activation
  callback must only enqueue work; LMS mutations belong to the room worker.
- The automated header latency budget is 100 ms, tested before a deliberately
  delayed 300 ms producer, with a separate 1000 ms first-body-byte budget.
  This measures the software HTTP path; it does not
  establish the maximum startup delay tolerated by physical Sonos hardware.
- The existing cached Python environment is used for regression tests. The
  LampaStream checkout is `/home/codex/src/LampaStream-consumer`, read-only for
  this task. No owner speaker, gateway, or deployment host is used by tests.

## Remaining integration

The production worker trims older history after observing a new audible parent,
preserving the audible parent URI and all continuation blocks. `WindowEdits`
refreshes Browse and fences each removal. Removed parents are invalidated and
their RAM and compressed disk chunks released immediately. The sink protects
two history parents, current and up to three produced upcoming parents; every
continuation of a protected parent shares protection. The default standalone
sink policy remains current/next for other callers. No LMS mutation is added.
Trims log ordinals, parent count, LMS indices and `reason=history-limit`.

The room worker still needs to publish the complete playlist window, determine
exact local lazy promises using the existing render/transition rules, bind
produced assets to immutable lazy routes and reconcile LMS navigation and
playlist changes through `WindowEdits`. Unknown/live duration must stop lazy
lookahead. It must add the single reason-checked lazy activation LMS selection
and permanent activation/binding/abandonment diagnostics. Existing prepared
production must bind with no LMS selection. The production checkpoint here
does not activate lazy routes and does not add app-driven LMS navigation.

Stage 2 remains app intent settling, matching retained ordinals to LMS indices,
one LMS selection for the final app target, restart and parent-relative seek,
and echo matching. Stage 3 is the owner checker; stage 4 is the feature README
and device qualification documentation. Those stages remain out of scope of
APPCONTROL-03. Complete stage 1 private LMS acceptance (lazy items, edits,
retained target navigation and continuation history) also remains outstanding.

ALAC bounds test failures came from its harness polling `error()` before
draining output, then accepting `done()` without rechecking the terminal error.
The decoder publishes error before completion. The corrected fixture checks
error after observing completion, matching the existing queue decoder policy;
no decoder/audio algorithm change is needed.
