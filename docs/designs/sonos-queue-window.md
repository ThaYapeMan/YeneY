# Sonos queue window implementation record

YENEY-APPCONTROL-03 is incomplete. The room relay uses the fenced editor for
natural history trimming and protects two history parents and their audio.
Lazy playlist publication is integrated by APPCONTROL-06 below; LMS navigation
reconciliation remains incomplete.
The APPCONTROL-04 partial checkpoint additionally connects read-only LMS status
to the production model for edits of already-published parents.
Next, Previous, restart and seek are not newly enabled. The README feature row
and step 3 qualification stay unchanged until controller integration and its
acceptance scenarios pass.

## APPCONTROL-06 production deferred headers

The worker now publishes missing upcoming parents with the fenced editor, up to
three in LMS playback order. DIDL retains rounded LMS duration and omits exact
size and sample frequency. A naturally prepared producer binds to that same URI
with zero LMS commands. Continuations remain part of their parent's block.
Two history parents remain protected; history selection is outside this task.

On a live activating PCM GET, the room worker claims the reader once and sends
one `playlist index` through `roomLmsCommand` with reason `lazy-pcm-get`.
The resulting start is assigned to the requested occurrence only when the LMS
snapshot confirms its index; an unrelated natural next start retains its normal
identity. The selection's flush/start echo resets the core producer while
preserving the owned speaker window. No echo issues another selection or
rebuilds the window. Binding uses the natural sink's actual immutable item:
STREAMINFO/M4A counts, rate conversion, transitions and carried prefixes are
unchanged. Finite remote sources keep their natural upper-bound promise;
unknown local counts keep sealed publication and may exceed the header deadline.
A late producer cannot revive a cancelled lazy route.

The header deadline is **3 seconds**, six times the previously measured 0.5 s
upper end of private LMS start times. It bounds the HTTP connection without
sending a partial header. This is a provisional software budget: physical Sonos
HTTP tolerance is an owner-run acceptance check, not established by loopback.
Expiry, reader abandonment and activation failure close without headers and log
numeric identities/counts, omitting source URLs, tokens and MACs. Existing exact
routes and the original declared lazy primitive retain their wire behavior.

Unbound HEAD and header-only probes return 503 without activation; malformed
ranges return 416. Unbound nonzero/suffix ranges return 503: their bounds cannot
be validated without an exact declaration, and no range probe may select LMS
speculatively. Initial PCM GET (no Range or a zero-based range including PCM)
activates; once naturally bound, HEAD and all ordinary ranges use the existing
exact response path. A missing/unknown LMS duration ends lookahead.

Owner-run later: request an unprepared item in the Sonos app while delaying LMS
start beyond three seconds; verify clean headerless disconnect, the speaker's
retry/skip behavior, one selection per live claim, no echo loop and the chosen
three-second budget within the device's actual HTTP tolerance. Also verify
normal app Next to a local, 96 kHz and ALAC lazy item. This task contacts no
owner speaker and does not modify the owner checker or feature qualification.

History selection (APPCONTROL-05 C), stage 1c, stage 2, the owner checker and
README/DEVICE-VERIFICATION remain separate work.

## Earlier checkpoints (historical)

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

The production worker now captures each producer's playlist occurrence token
alongside its LMS index. At the room poll interval it reads the full LMS status,
reconciles timestamp/shuffle/repeat and occurrence identities, then applies
fenced remove/insert edits to the published parents in the audible window.
Moving a parent reuses its original URI and byte-identical DIDL, including all
continuations; no audio is decoded again. Removed parents release audio and
their ownership claims. The audible URI is the edit anchor. If that occurrence
is deleted (including playlist clear), the worker logs the explicit missing
anchor decision, releases its owned queue and resets the relay without an LMS
mutation. Failed status reads preserve the previous publication.

This checkpoint does not publish absent upcoming parents: the natural producer
still supplies the published assets. Consequently the full six-slot publication
requirement of stage 1b remains open. Stage 1b must finish exact declarations,
lazy activation/binding, abandonment and temporary history-selection safety
before stage 1c adds LMS-originated navigation. Stage 2 remains app intent
settling, restart/seek and complete echo matching. The owner checker and feature
qualification documentation remain later stages.

The shared declaration calculation is now used by the natural sink. It keeps
the existing overlap subtraction and carried-prefix addition, and preserves the
finite remote duration plus margin. Source-rate conversion delegates to the
core's decimation policy and rounds exact source counts up after division.
The declaration fixture compares eight per-item WAV headers, Content-Lengths
and DIDLs with naturally published sink items: six source rates, an outgoing
transition and a carried prefix. It does not establish production lazy metadata
probing, ordinal publication or activation/binding acceptance.

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

The lazy request primitive now fences worker activation against live PCM readers.
The worker must call `LazyItem::claimActivation()` immediately before issuing
its selection command; a queued HTTP callback alone is insufficient. The claim
checks peer close and cancellation again, so a disconnect need not wait for the
resolver's 50 ms polling interval. The final departing reader cancels an unbound
item, including on timeout or enqueue failure; a late producer cannot revive it.
One surviving concurrent range reader preserves a single activation claim.
This lifecycle fence does not connect production lazy publication or LMS
selection, and does not complete stage 1b.
