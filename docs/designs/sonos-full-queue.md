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

This implementation is not full acceptance of FULLQ-01b. The inherited fake's
600 ms delay applies only to Browse and topology reads. Under that weaker model,
12/100/500-row builds measured 16.572/28.089/131.352 seconds. Album first-byte
latency remained about 14.05 seconds, and generation replacement about 8 seconds.
Those album-start results miss the 1.5-second target. FULLQ now also supports a
600 ms delay on every SOAP call; results from the two models must not be mixed.

There is also a discrepancy between Q8's single replacement and the existing
B1/B2 history contract. The immutable original full resource, seek suffix and
frame-zero history route require two row replacements over that lifecycle:
full row to suffix, then suffix to deferred frame-zero history. Both affect only
that row and both log `reason=seek-partial edits=2`. Omitting the second edit by
rebinding the suffix URL would repeat the representation violation rejected in
FULLQ-01. Passing B1/B2's PCM and switch assertions does not establish Q8's
single-replacement requirement. See the report for final gate status.

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
batch-rejection fallback and the same complete mirror. Q8 still records two
row replacements, so the requested single-replacement lifecycle is not met.

Confirmed serial AddURIToQueue, SetAVTransportURI and Play already require
1.8 seconds with 600 ms on every call. The 1.5-second cold-start target cannot
be reached with that serial transport contract, even before ownership fences.
This is an unmet requirement, not an accepted relaxation of the target.

The final full regression gate was terminated twice by SIGTERM, before an
assertion failure in either interrupted run; no full exit-0 result is claimed.
All three final soak seeds passed with zero ordinary navigation edits and zero
seek-partial replacements. The focused navigation/remaining regression tail and
the read-only require_v1 consumer check passed. Full details are in the
FULLQ-01b report. No owner LMS or speaker was contacted, and no deployment
occurred.
