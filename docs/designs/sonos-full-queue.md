# Static Sonos playlist mirror — round 1

The finite LMS playlist owns a stable ordered row for each occurrence in its
first `YENEY_QUEUE_ROWS` entries (default 500). Ordinal i denotes LMS index i-1.
A duplicate track has a distinct occurrence token. Continuous sources retain
the stream path; finite remote items and continuations retain their existing
upper-bound declarations and parent-relative clocks.

## Publication and navigation

Start the current row before building the rest. Publish lazy rows with LMS
DIDL title, artist, album and duration, omitting size and sampleFrequency before
production. Batch consecutive insertions in groups of at most 16, with a fresh
Browse/UpdateID fence and exact URI-vector confirmation. A definitive SOAP
fault disables batching for the room and logs one fallback; uncertain transport
failures do not retry and duplicate rows.

Build once per playlist generation; a generation change may release/rebuild
and logs its edit count. Beyond-cap starts use the previous window path and log
`window rebuild reason=beyond-cap`. More sophisticated changes are deferred.

LMS Next, Previous, jumps and repeat-wrap select an existing row with TRACK_NR,
then Play only if needed. App selections pass through the existing index-keyed
arbiter and serialized LMS command outbox. A self-originated TRACK_NR is fenced
by index, token, URI, ordinal, count and transport identity. Normal navigation
has zero queue edits. PCM consumers activate lazy production, while HEAD and
invalid/header-only requests cannot activate it. Each move reports its pairing
or reasoned reproduction decision.

## Audio retention and immutable resources

Rows are independent of audio retention. Protect current plus next three
produced parents, retaining the existing local 256 MiB / finite-remote 8 GiB
quotas. Evict oldest played audio first. An evicted route remains a row and
reproduces on later Back with `decision=re-produced reason=evicted`.
Eviction is distinguished from source and worker faults.

All I1-I6 invariants in CLAUDE.md continue to apply. In-item seeks require a
known landing and exact 4096-frame overlap, including conversion warm-up and
known retained WAV origin. Speaker rewinds retain the existing selection-based
clock authority. Continuations keep immutable declarations and parent identity.

Round 1 permits the existing seek-partial row replacement when verification
cannot attach to the retained WAV. Each replacement is fenced and logs
`window replace ordinal=N reason=seek-partial edits=2`. A suffix URI cannot be
rebound to a full frame-zero resource. History uses a distinct deferred
frame-zero route, preserving B1/B2/F3's command, PCM and single-switch checks.
Round 2's sparse full-length resources remove this exception and define cap
policy. Playlist diffs and SaveQueue changes remain later work.

## Acceptance and measurements

Acceptance is measured against disposable loopback LMS and the fake renderer,
never the owner's LMS or speakers. The renderer's calibrated latency model and
actual byte-read timestamps must be stated with results; CLI convergence plus
a delay is not an audible measurement. See the FULLQ-01b report for measured
results and any unmet requirements. A passing basic ordering check alone does
not establish latency, cache eviction, generation rebuild or scale acceptance.

## Decision history

FULLQ-01 stopped at immutable partial/history representation. The owner
resolved that stop for round 1 by permitting the existing partial replacement.
The model and batch commits were reapplied, followed by the saved experiment;
the rejected suffix-to-full URI rebinding is excluded for partial history.
The core pin remains `9d5aa42`; no core change is required or included.

### Acceptance limitations found in FULLQ-01b

Historical FULLQ-01b status (superseded by the FULLQ-01c measurements below): The inherited fake's
600 ms delay applies only to Browse and topology reads. Under that weaker model,
12/100/500-row builds measured 16.572/28.089/131.352 seconds. Album first-byte
latency remained about 14.05 seconds, and generation replacement about 8 seconds.
Those album-start results miss the 1.5-second target. FULLQ now also supports a
600 ms delay on every SOAP call; results from the two models must not be mixed.

There was also a discrepancy between Q8's original single replacement and the existing
B1/B2 history contract. The immutable original full resource, seek suffix and
frame-zero history route require two row replacements over that lifecycle:
full row to suffix, then suffix to deferred frame-zero history. Both affect only
that row and both log `reason=seek-partial edits=2`. Omitting the second edit by
rebinding the suffix URL would repeat the representation violation rejected in
FULLQ-01. The owner corrected Q8 in FULLQ-01c: exactly two such replacements and no
other replacements are required. B1/B2 PCM and switch assertions are retained.

The final regression fixes also fence asynchronous producer admission by token
and a producer lower bound, retire superseded app selections only when their
cached resource is complete, and release RAM as well as compressed history on
cache eviction. A continuous source divides finite queue ownership; a later
finite segment must contain its current item rather than reuse an earlier
segment's prefix. A focused model assertion covers that boundary.

With the strict model, the measured FLAC experiment built 12/100/500 rows in
34.010/64.628/215.870 seconds, with album first-byte times around 28.5 seconds.
Strict navigation deadlines also failed in some moves. These timing runs
precede the last admission safety fixes and do not establish final acceptance.
Earlier measurements timestamped the first HTTP body byte, including the WAV
header. The final measurement helper timestamps the first PCM byte after the
44-byte header and still verifies the target audio bytes.
`make full-queue-acceptance` reproduces the stronger codec/scale and exception
checks separately from the regression gate; it is currently failing.

### Final strict measurements

These observations use the final production binary (SHA256
`48637fba04f85c0bba2b0cc5f68c862ff9a333e8f2556c0ade2a3872b82cf4f0`),
600 ms on every SOAP call, and the first PCM byte after the WAV header.
Exact PCM identity is checked separately from timing.

| Source | Rows | First PCM (s) | Complete build (s) |
|---|---:|---:|---:|
| FLAC | 12 | 28.450 | 34.473 |
| FLAC | 100 | 28.543 | 56.245 |
| FLAC | 500 | 28.475 | 214.774 |
| ALAC | 12 | 28.608 | 34.714 |
| ALAC | 100 | 28.633 | 64.955 |
| ALAC | 500 | 28.406 | 214.816 |
| WAV | 12 | 29.579 | 35.603 |
| WAV | 100 | 29.554 | 59.816 |
| WAV | 500 | 29.558 | 209.763 |

All nine strict timing cases fail acceptance. The FLAC 12-row run measured
LMS moves from 1.362 to 2.514 seconds and app moves from 0.001 to 1.363 seconds.
Q4 confirms deep app/LMS Back and exact reproduction after eviction under a
24 MiB quota. Q6 confirms a generation rebuild and zero-edit navigation
thereafter, but generation start takes 24.170 seconds. Q7 confirms one logged
batch-rejection fallback and the same complete mirror. Q8 recorded two row replacements. FULLQ-01c corrects the owner requirement
to accept exactly those two immutable-resource swaps.

Confirmed serial AddURIToQueue, SetAVTransportURI and Play already require
1.8 seconds with 600 ms on every call. The 1.5-second cold-start target cannot
be reached with that serial transport contract, even before ownership fences.
FULLQ-01c classifies this 600 ms model as stress only; device acceptance uses
30 ms SOAP responses. The actual device targets remain unchanged.

The final full regression gate was terminated twice by SIGTERM, before an
assertion failure in either interrupted run; no full exit-0 result is claimed.
All three final soak seeds passed with zero ordinary navigation edits and zero
seek-partial replacements. The focused navigation/remaining regression tail and
the read-only require_v1 consumer check passed. Full details are in the
FULLQ-01b report. No owner LMS or speaker was contacted, and no deployment
occurred.

## FULLQ-01c device calibration and results (13 November 2026)

Acceptance uses `YENEY_FULLQ_PROFILE=device`: 30 ms per SOAP response, 400 ms
from a transport move to fetching PCM, and 50 ms per GENA echo. The owner’s
12 November Study journal showed consecutive SOAP spacing of 7–45 ms and
200–450 ms TRANSITIONING→PLAYING. `strict` keeps 600 ms per SOAP as stress;
`inherited` preserves `YENEY_FULLQ_DELAY` and `YENEY_FULLQ_ALL_SOAP` behavior.
Profiles and their origin are documented in `tests/full_queue_real_lms.py`.

Calls are captured at the fake SOAP handler and filtered by its first PCM-byte
read timestamp. Latency is trigger to first PCM byte, with exact PCM identity
checked. Polling reads remain in the reported lists. All tests use disposable
loopback LMS and renderer endpoints.

Per-operation SOAP contract: LMS Next/Previous/jump/repeat-wrap issues exactly
one `Seek TRACK_NR`, plus `Play` only when not playing; no Browse, topology
read, add or remove. App Next/Back/tap issues zero SOAP writes and one LMS
`playlist index`. In-item seeks issue one `Seek REL_TIME`, zero edits.

Ownership now reuses one acquisition topology read. Live GENA grouping and
foreign transport observations still block writes; release uses fresh fences.
Backup creation, verification, queue identity and every real-edit fence remain.
Backup retention moves after Play, where it overlaps the transport transition,
and completes before subsequent navigation. It can therefore appear in the
cold call list before the first byte without delaying admission.

The measured baseline has one Get/Set/Get crossfade sequence, not two Get/Set
pairs. The first read saves the original setting; the last verifies the write.
Both reads and the single write remain; removing verification would weaken
admission safety. No duplicate crossfade pair was found to trim.

Cold-start call lists for the 12-row FLAC case, in full:

Before (`b52323e`):

```text
SOAP album_start ["GetZoneGroupState", "Browse", "GetMediaInfo", "GetZoneGroupState", "SaveQueue", "Browse", "Browse", "Browse", "GetZoneGroupState", "DestroyObject", "Browse", "Browse", "GetZoneGroupState", "DestroyObject", "Browse", "Browse", "GetZoneGroupState", "DestroyObject", "Browse", "GetMediaInfo", "GetPositionInfo", "GetMediaInfo", "GetZoneGroupState", "SetAVTransportURI", "GetMediaInfo", "GetCrossfadeMode", "GetZoneGroupState", "SetCrossfadeMode", "GetCrossfadeMode", "GetPositionInfo", "GetMediaInfo", "GetZoneGroupState", "RemoveTrackRangeFromQueue", "Browse", "GetPositionInfo", "GetMediaInfo", "GetZoneGroupState", "AddURIToQueue", "Browse", "GetZoneGroupState", "Seek TRACK_NR", "GetPositionInfo", "GetMediaInfo", "GetZoneGroupState", "Play", "GetPositionInfo", "GetTransportInfo", "GetMediaInfo", "Browse", "Browse", "GetPositionInfo", "GetMediaInfo", "GetZoneGroupState", "AddMultipleURIsToQueue", "Browse"]
```

After (`FULLQ-01c`):

```text
SOAP album_start ["GetZoneGroupState", "Browse", "GetMediaInfo", "SaveQueue", "Browse", "Browse", "GetMediaInfo", "GetPositionInfo", "GetMediaInfo", "SetAVTransportURI", "GetMediaInfo", "GetCrossfadeMode", "SetCrossfadeMode", "GetCrossfadeMode", "GetPositionInfo", "GetMediaInfo", "RemoveTrackRangeFromQueue", "Browse", "GetPositionInfo", "GetMediaInfo", "AddURIToQueue", "Browse", "Seek TRACK_NR", "GetPositionInfo", "GetMediaInfo", "Play", "Browse", "Browse", "DestroyObject", "Browse", "Browse", "DestroyObject", "Browse", "Browse", "DestroyObject", "GetPositionInfo", "GetTransportInfo", "GetMediaInfo", "Browse"]
```

Device latency matrix (seconds; ranges cover every tested move):

| Codec | Rows | Start before → after | LMS before → after | App before → after | Build before → after |
|---|---:|---:|---|---|---:|
| FLAC | 12 | 2.035 → 1.387 | 0.592–0.639 → 0.584–0.596 | 0.401–0.429 → 0.400–0.407 | 2.079 → 1.614 |
| FLAC | 100 | 2.055 → 1.397 | 0.603–0.835 → 0.591–0.804 | 0.401–0.403 → 0.400–0.501 | 3.192 → 2.604 |
| FLAC | 500 | 2.022 → 1.413 | 0.662–0.690 → 0.623–0.652 | 0.401–0.515 → 0.401–0.524 | 11.177 → 9.801 |
| ALAC | 12 | 3.169 → 2.563 | 1.791–1.897 → 1.751–1.856 | 0.401–1.590 → 0.401–1.568 | 3.212 → 2.793 |
| ALAC | 100 | 3.156 → 2.551 | 1.790–2.099 → 1.745–2.062 | 0.401–1.589 → 0.400–1.604 | 4.288 → 3.742 |
| ALAC | 500 | 3.174 → 2.560 | 1.823–1.943 → 1.778–1.918 | 0.401–1.669 → 0.401–1.634 | 12.310 → 10.958 |
| WAV | 12 | 4.285 → 3.686 | 0.626–3.120 → 0.592–3.288 | 0.401–2.672 → 0.401–2.677 | 4.346 → 3.882 |
| WAV | 100 | 4.293 → 3.689 | 0.632–3.128 → 0.600–3.388 | 0.401–2.691 → 0.401–2.695 | 5.552 → 4.883 |
| WAV | 500 | 4.316 → 3.704 | 0.663–3.161 → 0.637–3.125 | 0.401–2.850 → 0.401–2.767 | 13.669 → 12.274 |

FLAC passes all device deadlines. Tail-moov ALAC and raw PCM WAV fail the
unchanged C2 deadlines. The original generated ALAC file is 4,236,546 bytes,
with moov at 4,234,684 and mdat at 36. A diagnostic remux puts moov at 28
and mdat at 1,898, preserving PCM and the 255 KiB threshold. Faststart ALAC
measures 1.397 s album start, 0.596–0.598 s LMS moves and 0.400–0.401 s app
moves. Tail-moov ALAC measures about 1.74–1.86 s strm-to-byte on navigation;
faststart about 0.58 s. This isolates metadata-at-EOF ingestion in the pinned
decoder, rather than threshold or mirror batch size. It is documented rather
rewritten by this task: core changes and media mutations are excluded.

WAV is sent by LMS as raw wire codec p with threshold 255 KiB. It has no exact
source-frame descriptor for progressive publication; the immutable local WAV
promise remains sealed until EOF. Uncached rows thus cost about 3.1 s; already
cached moves cost about 0.6 s. No duration estimate is substituted for an exact
frame count. Fixing this needs a separate source-length/core design decision.

Q8 now requires exactly two `window replace … reason=seek-partial edits=2`
lines for full → suffix → frame-zero history, and zero other replacements.
This is the only navigation path with row edits until sparse items land.
B1 retains byte identity, one LMS command, zero renderer Back seeks and no
switch to the old partial. The old one-swap assertion fails with count 2.

Full-gate result and all individual operation timings are recorded in
`/home/codex/src/reports/yeney-fullq-01c-report.md`. Outstanding acceptance
limits remain ALAC tail metadata and raw WAV C2 timing; stress timings are
historical evidence, not the device acceptance model.

The device-profile Q4 quota test also exposes a baseline pressure/navigation race.
It reproduces with the unchanged b52323e binary and original Q4 test source.
On the trimmed binary, a deep Back can retain an owned-selection ticket and
reach the lazy deadline without its LMS command. No outbox or mirror model
change is included in this measurement/trimming round; Q4 device acceptance
remains outstanding. Q6 generation rebuild and Q7 batch fallback pass.
