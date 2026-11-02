# Sonos queue window implementation record

YENEY-APPCONTROL-03 is incomplete. The room relay uses the fenced editor for
natural history trimming and protects two history parents and their audio.
Lazy playlist publication is integrated by APPCONTROL-06 below; LMS navigation
reconciliation is integrated by APPCONTROL-07 (stage 1c).
The APPCONTROL-04 partial checkpoint additionally connects read-only LMS status
to the production model for edits of already-published parents.
App history selection, app Previous, restart and seek remain later work. The README feature row
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

History selection (APPCONTROL-05 C), stage 2, the owner checker and
README feature qualification remain separate work.

## APPCONTROL-07 LMS navigation (stage 1c)

An LMS controller navigation sends a flush/start pair, whereas natural
decode-ahead sends another start without the flush. Previously `command('q')`
unconditionally called `release`, discarded the published parents and advanced
`firstStream`; `selectedNewPlay` then started the new queue at ordinal one.
This is earlier than the missing-audible-anchor fallback. It explains why a
mid-track user Next lost history while natural advance preserved it.

The room worker keeps ownership for a 250 ms flush/start settle interval and
marks the next producer as an LMS navigation. A flush without a paired start
still honours an explicit stop. The next start reads status and is
assigned to the confirmed status index itself, not the natural decode-ahead
`index + 1`. Old speaker evidence is fenced until that producer has a natural
immutable declaration. No LMS mutation is issued. App lazy activation keeps
its existing single-command echo path.

Reconciliation expands the new target's two-history/three-upcoming span using
retained parents and new lazy upcoming declarations. A fresh target declaration
can replace only its parent block; other overlapping blocks keep their original
URIs, DIDL and continuation audio. The fenced editor uses a private plan identity
for a changed declaration and anchors on an unchanged common parent. Complete
retained targets with matching length and zero source offset reuse their audio.
The speaker is sought to the resulting current ordinal after edits finish.

Only existing produced audio can supply history. A placeholder does not become
history merely because a jump crosses it. Interrupted production preserves the
existing PCM prefix and immutable promise; a read into the unavailable suffix
fails, without padding or fabricated audio. Explicit window protection keeps
published audio protected even when producer chronology changes after Previous.
Removing a parent releases its retained audio.

If no retained parent bridges the windows, the same fenced editor replaces the
parent vector and logs `window rebuild reason=out-of-range lms_commands=0`
(or `reorder` when shuffle changes). Clear/missing current identity uses the
explicit owned-queue release with `clear`/`reorder`. No unrelated queue is
flushed. Navigation diagnostics are `queue window lms-nav from_index=<i>
to_index=<j> action=<slide|rebuild> history=<n> lms_commands=0 kind=<kind>`;
they contain no source URL or token. Natural boundaries also log their slide.

Private LMS 9.1.1 tests use a fake Sonos speaker and an eight-entry album,
checking user Next, Previous, seek, distant jump, retained PCM/URIs and zero
bridge LMS selections. Physical speaker behavior remains an owner check.
App history selection/tapping a played row and app Previous are deferred,
as are stage 2 intent settling and parent-relative app seek.

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

## Earlier integration notes (superseded by APPCONTROL-06/07 above)

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

## Queue URL device evidence (1 November 2026)

The Sonos app showed every YeneY queue row blank (no title, artist or artwork), although the speaker stores complete metadata (Browse Q:0 returns correct dc:title, artist, album, albumArtURI). Owner probes replayed YeneY's exact stored DIDL from a neutral HTTP server, changing one factor at a time:

| Probe | What differs from YeneY | App rows |
|---|---|---|
| res attributes (with/without size and sampleFrequency) | — | titles shown |
| HEAD/non-zero Range answered 503 | — | titles shown |
| all items share one DIDL item id vs unique ids | — | titles shown |
| YeneY's exact DIDL, added in one batch, URL `/replay-N.wav` | content identical | titles shown |
| same, inserted one at a time at explicit positions | edit pattern | titles shown |
| same, plus a RemoveTrackRangeFromQueue history trim | edit pattern | titles shown |
| **same, URL `/music/queue.wav?session=…&epoch=1&item=N&revision=1`** | **URL shape only** | **blank rows** |

So the URL shape alone blanks the rows: either the `/music/` path prefix or the query string (or both). Other device facts from the same probes: a Sonos never sends HEAD; it fetches the next item about 6 s before the current one ends with a plain GET from 0; after a skip into an item it already prefetched, it sends GET from 0 and then `Range: bytes=<buffered>-`.


All queue items now use `/yeney/q/<session>/<epoch>/<revision>/<item>.wav`. Stream CurrentURI remains unchanged. Old query routes are removed: the session and queue assets do not survive process restart. HTTP fencing, declarations and audio are unchanged.
