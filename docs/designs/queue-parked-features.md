# Queue full-v1 preservation and rebuild checklist

The immutable annotated `queue-full-v1` tags preserve bridge
`98727dbfede340e11a3ade9f3686db2c73ca4165` and core
`13db8b1d210f0e24b2fb29ff5e2408330062b044`. Both tags were pushed before
relay changes. Inspect a file with `git show queue-full-v1:path`; use
`git log queue-full-v1 -- path` for its complete implementing history.
No GPL/LGPL reference implementation is incorporated into YeneY.

The relay rebuild deliberately keeps finite WAV rendering, exact length and
seek-origin accounting, decoded/audible separation, speaker-confirmed STMs,
STMs-before-STMd, remote successor delay, core crossfade/tail recovery,
item storage/history, finite HTTP Range routes, metadata/artwork sanitisation,
QueueActions, verified foreign backup and crossfade restoration.

## Rebuild order and feature inventory

| Step | Parked feature and behaviour | Files at queue-full-v1; implementing commits | Preserved tests |
|---|---|---|---|
| 2 | Prepare → active → drain → handoff, audible tail preservation, same-position stream continuation, measured gap and session latch until stop/new play | `room_mode.*`, `mode_sink.h`, `queue_room.cpp`, `playback_mode.h`; `1806e22`, `32008e7` | `room_mode_test.cpp`, `queue_mode_test.py`, `queue_guard_stream_test.py`, `queue_binary_test.py` |
| 2 | Automatic fallback: foreign-control, preparation-timeout, soap-error, length-fault, storage-fault, coordinator/grouping-change, lms-unavailable, rearm-loop; one release/request per session | `room_mode.*`, `queue_controller.*`, `queue_room.cpp`, `queue_lms_client.h`; `1806e22`, `b1da8d8`, `77535ad` | room-mode, controller, mode, binary and LMS-client tests |
| 2 | Epoch/generation fencing of producers, sinks, late HTTP reads and speaker events; readiness/rearm continuation and rearm-loop guard (more than three changes in five seconds) | `mode_sink.h`, `queue_room.cpp`, `queue_route.*`, core `core/sink.h`, `core/queue_mode.*`, `core/player.cpp`; bridge `1806e22`, `77535ad`, `22bf3c0`, `62449bb`; core `1426efd`, `2bddbf8`, `4bdc5c3` | `room_mode_test.cpp`, `queue_mode_test.py`, core `speaker_test.py`, `queue_test.py` |
| 3 | App pause/resume, prepared next/previous and seek inferred from GENA/GPI/GTI/UpdateID, own-command matching, CLI echo suppression, ambiguous action release | `queue_controller.*`, `queue_room.cpp`; `8b498b6`, `31507e7` | `queue_app_control_test.py`, `queue_audible_test.py`, `queue_binary_test.py` |
| 3 | All LMS mutations through one reason-checked choke point; verified asynchronous commands, fresh connections, safe query retry, slow/failure counters and 30-second outage policy | `queue_lms_client.h`, `queue_lms_cli.h`, `queue_room.cpp`; `b1da8d8`, `33adfd8` | `queue_lms_client_fixture.cpp`, `queue_lms_client_test.py`, `queue_app_control_test.py` |
| 3 | Read-only LMS listener for in-track seeks, own-seek suppression, retained-history REL_TIME versus fresh render, cached mixed intro on manual skip | `queue_room.cpp`, `queue_controller.*`, `core_queue_sink.*`; `c7af8c3`, `4982233` (core), `1897548` (core) | `queue_lms_control_test.py`, `queue_app_control_test.py`, core speaker/queue tests |
| 4 | Whole-list occurrence-token reconciliation, at least three prepared ahead, safe future-only add/delete/move/clear, history re-add, mixed-intro caching | `queue_controller.*`, `queue_playlist.h`, `queue_room.cpp`; `b68cb0e`, `c7af8c3` | `queue_controller_test.py`, `queue_lms_control_test.py`, `queue_binary_test.py`, `queue_controller_fixture.cpp` |
| 4 | Batch enqueue helper (not production), reorder/remove planning and UpdateID/URI fencing | `upnp/queue_actions.*`, `queue_controller.cpp`; `b68cb0e`, `38b5e40`, `a2a93c1` | `queue_actions_test.cpp`, controller tests |
| 5 | Group/coordinator lease checks, standalone eligibility, deferred auto mode and group-rate policy | `upnp/queue_coordinator.*`, `playback_mode.h`, `queue_room.cpp`; `b68cb0e`, `32008e7` | `playback_mode_test.*`, `queue_guard_stream_test.py`, controller/binary tests |
| 2–5 | Advanced status: requested/active mode, prepare/drain/handoff, reason, epoch/generation, occurrence, ownership, ahead, rates, promised/actual frames, latch, fallback/gap, LMS CLI counters | `room_mode.*`, `stream_metrics.h`, `sonos-status.cpp`; `1806e22`, `b1da8d8` | `room_mode_test.cpp`, `queue_mode_test.py`, `queue_lms_client_test.py` |

Generation-only core hooks to revisit in step 2: `Sink::playbackGeneration`,
`Sink::playbackReady`, QueueMode::pollGeneration/beginRun and its
`modeGeneration`. Received/cached-start authorization and retained track state
also supported step-3 refetch. These hooks must not drive the relay or issue a
HELO. Existing core rendering policy remains independently tested.

## Owner suite scenarios parked with the old implementation

At the tags, `scripts/yeney-queue-device-suite`, `scripts/yeney_queue_ws.py`,
`tests/queue_device_suite_test.py`, `tests/queue_device_cli_test.py` and
`tests/queue_device_ws_test.py` preserve the complete unattended suite.
Step 2 restores fallback and stream-reference comparisons; step 3 restores
next/previous, history/beyond-history seeks and the optional app checklist;
step 4 restores ALAC/FLAC cross-album 10-second crossfade and mixed-rate
playlist acceptance. Step 5 adds group/auto qualification. Stop/new-play and
LMS pause/resume stay in the relay suite. The read-only TLS WebSocket 5 Hz
position experiment, 60-second stream reference, fit/residual report and
non-fatal WS verdict belong to step 2 diagnostics, not relay correctness.
Keep their standard-library framing, message-shape, unavailable/whole-second/
sub-second/inflated-after-error tests when restoring them.

## Device evidence and cautions

* 12 October Study probe proved finite 24-bit WAV, AddURIToQueue, queue URI,
  Seek TRACK_NR 1 and Play. These are the relay building blocks.
* 20 October, c617bb1: SaveQueue on an empty Q:0 returned HTTP 500 before
  scenarios. bcbfc6c made backup conditional on foreign entries.
* 20 October 14:17, bcbfc6c: slow playlistcontrol load timed out; late replies
  poisoned the connection and restoration lost a 14-track playlist and
  transitionSmart. b1da8d8 added fresh connections, identity restoration and
  per-item best effort. Preserve these suite safeguards.
* 20 October 16:13, b1da8d8/core b8e5fbc: internal generation reset emitted
  HELO and caused 36 LMS q/s restarts in three seconds. Core 2bddbf8 removed
  the internal reconnect. Never reintroduce session HELO.
* 20 October 18:00, 77535ad/core 2bddbf8: readiness `playlist index` itself
  caused q/s and rearms; loop guard fell back. 62449bb/33adfd8 removed internal
  LMS mutations; b544983/core 13db8b1 completed strict queue handshakes.
* 20 October 23:14, b544983: AddMultipleURIsToQueue fault 402. 38b5e40 switched
  to device-proven AddURIToQueue. Its rejection remains unexplained and may
  have been the invalid album XML seen next morning; do not treat batches
  as device-qualified. Remote negative IDs changed on restore; compare URL.
* 21 October 08:57: AddURIToQueue 402 contained album U+0016 followed by
  “Het beste uit pop in je moerstaal, Vol. 2”. b74e689/a7aace5/81ef410 fixed
  outgoing XML, consistent display caching and strict fake replay.
* 21 October 10:16, fb4b589: accepted enqueue, Play, audible in 1.287379 s,
  no fallback. Suite falsely rejected sanitised album identity; 1cbf83f and
  98727db fixed expected XML/title-format handling.
* 21 October 16:06, 98727db: audible in 1.620200 s, owned at 16:06:48, but
  suite status still said prepare/owned=false and its three-second check
  failed. Relay status must publish with the speaker observation itself.

These runs qualify the rendering and SOAP primitives, not the advanced
controller. Rebuild each parked component independently against real LMS
and the strict fake before requesting another owner run.

## Relay extraction record

The relay removes the advanced source/test targets listed above rather than
leaving a second controller active. `tests/queue_sonos_fake.py` now owns the
shared strict SOAP fake extracted from the old controller tests. Metadata and
stream-wire tests retain that fake without the parked controller driver.
`queue_playlist.h` keeps only metadata parsing; duplicate occurrence matching,
revision state and ambiguous-edit rejection are parked with step 4.
Core `modeGeneration` and its producer-reset/readiness branch are removed;
public generation hooks retain inert defaults for source compatibility. Only
the generation-specific block in core `tests/speaker_test.py` is retired.
Core render/refetch tests remain useful independent tests of retained rendering.
The new storage-completion gate prevents decoder EOF from admitting a successor
before all promised WAV frames exist. The old store descriptor/route argument
names retain their compatibility meaning; the relay uses a constant namespace,
not a mode epoch.
