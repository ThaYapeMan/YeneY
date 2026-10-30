# Sonos queue window implementation record

YENEY-APPCONTROL-02 is in progress. The existing room relay does not yet call
these building blocks. Next, Previous, restart and seek are not newly enabled
by this change. The README feature row and step 3 qualification stay unchanged
until the controller integration and acceptance scenarios pass.

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

The room worker still needs to publish the window, determine exact local lazy
promises using the existing render/transition rules, bind produced assets,
protect window audio under the shared quota, and reconcile LMS navigation and
playlist changes through `WindowEdits`. App intent settling, echo matching,
retained history navigation, restart/seek, the owner checker, private real LMS
acceptance and the requested feature documentation are still outstanding.
