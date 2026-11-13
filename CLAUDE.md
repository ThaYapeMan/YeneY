# CLAUDE.md

## What this is

Registers Sonos speakers as LMS players; synchronised playback with other LMS players is not yet accurate. Not an LMS plugin — a standalone program using yeney-core with a Sonos network sink.

## Build environment

The WSL2 repository checkout is the development environment. Build locally with `make` and run automated tests with `make test`. The Makefile builds the core and project-owned bridge with current compilers.

LXC 113 (192.168.178.31) is deployment only. Agents must never SSH to it or deploy there. After the agent commits and pushes, the user runs `git pull && git submodule update --init --recursive && make && sudo make install` on the deployment target. The installer restarts the configured room services listed in `/etc/yeney/rooms`.

## Submodules

- `third_party/yeney-core` is the player implementation; its recursive Apple ALAC module must be initialized.
- Never rewrite published history or discard uncommitted submodule work. Commit and push intentional submodule changes before updating their pins.

## UPnP layer

YeneY is the only backend. `YENEY_UPNP=yeney` and `own` are accepted; other values warn and continue with YeneY. Keep HTTP Server constants, wire fixtures, URLs, transport ordering and timing unchanged unless the task explicitly requires a change. Queue item URLs use `/yeney/q/<session>/<epoch>/<revision>/<item>.wav`; the Sonos app blanks rows for the old `/music/queue.wav` query shape. Live/stream URLs are unchanged. The encoded buffer capacity is in packets, not bytes.

## Our modifications

- **Podcast metadata**: `podcast_settings.h` resolves environment, room config, global config and defaults (`YENEY_PODCAST_TITLE_PLAIN=yes`, `YENEY_PODCAST_ARTIST_SHOW=yes`, `YENEY_PODCAST_ALBUM_PUBLISHER=no`). The bounded matching feed/menu lookup handles plain native titles and decorated feed names, resolves formatted by/from splits against episode/menu data, and caches title, podcast and publisher; incomplete cache entries trigger a fresh lookup. Step/reason diagnostics omit URLs and tokens. Own LMS fields win. Disabled or unknown fields are sent as empty elements; a Sonos discards DIDL without upnp:album. Sanitised logs/status match the sent values, without LMS display mutation. Direct starts work without systemd.

- **Track metadata**: LMS CLI (port 9090) is queried for title, artist, album and artwork at stream start. `YENEY_TITLE_FORMAT=artist-title|title` formats the stream-path sent title only (default artist-title), because radio-type DIDL shows only the title. Queue musicTrack items always send the plain sanitised LMS title (empty stays empty), with separate artist/album fields; keep the sent-title log and cached renderer title consistent with DIDL. Continuous-stream status alone follows confirmed LMS track changes (title and duration) without rewriting DIDL. `scripts/meta-variant.py` is an owner-run device diagnostic.
- **MAC address**: sent raw, not URL-encoded. URL-encoding causes LMS to silently fail to match the player.
- **Artwork URL**: local, non-remote tracks keep `http://<lms>:9000/music/<id>/cover.jpg` because LMS does not include `artwork_url` for them. Remote tracks use `artwork_url` after exactly one CLI decode: relative paths are prefixed with `http://<lms>:9000`, including existing `/imageproxy/` paths without decoding their inner escapes. Absolute HTTP/HTTPS artwork is percent-encoded through the LMS `/imageproxy/<URL>/image.jpg` endpoint. Missing remote artwork omits `albumArtURI`. Both modes use `lms_artwork.h`; DIDL XML-escapes the resolved URL.
- **Player name**: the true name is always `<room> (Sonos)` (e.g. `Study (Sonos)`), with unchanged room and MAC identity. The display name alone is sent as the SlimProto player name. `--show-sonos-suffix=yes|no` overrides per-room `YENEY_SHOW_SONOS_SUFFIX`, then the global key of that name in `/etc/yeney/config` (`YENEY_CONFIG` override), then default `yes`. Invalid values warn once and use yes. All sources work without systemd. LMS renames last until the next restart; SETD reapplies the configured display name. Logs/status retain the true name; status also publishes display_name and mac. Tools identify by status MAC, never display name.

## Rejected approaches

**ICY/Shoutcast in-band metadata** — 28 September device probes show that the CurrentURI scheme selects the client: `http://` does not request ICY and plays FLAC; `x-rincon-mp3radio://` requests ICY for MP3 and FLAC but cannot play FLAC. Keep the bridge's HTTP CurrentURI with radio `audio/flac` protocolInfo. MP3 ICY StreamTitle reaches streamContent, but the current app still shows only the title. See README for details.

## Diagnostics

The program line-buffers stdout, including under systemd. Use `journalctl` for transport/stream logs and `ss -tnp` for connection state. See `DEVICE-VERIFICATION.md` for the physical-device acceptance checks.

## Pitfalls

- Never pass `--server=<ip>:9000`; port 9000 is the web interface. Pass `--server=<ip>` only — slimproto is found on port 3483.

## Conventions

- All documentation and commit messages in English.
- Always push after committing.

- Runtime state, settings and logging must also work when the binary runs directly,
  without systemd, for a future LMS plugin. No systemd-only runtime dependencies.

## Per-room volume

`YENEY_LMS_VOLUME=off|speaker` defaults to off. `--lms-volume` overrides the
process environment, which overrides `room.<name>.lms_volume` in the global
config. `YENEY_VOLUME_MAX` / `room.<name>.volume_max` caps writes (default 100).
`volume_control.h/.cpp` owns settings and debounce/echo state;
`volume_worker.h` performs bounded reads and speaker writes outside the audio
and relay workers. All LMS mutations still pass `roomLmsCommand`.
Read exact LMS mixer percentages; never infer them from audg or scale PCM.
Coordinators use GroupRenderingControl; standalone/member rooms use their own
RenderingControl. Off mode preserves existing network behavior. The private
LMS 9.1.1 rendered Audio settings test proves native digitalVolumeControl is
not exposed for Model=yeney (HasDigitalOut defaults to 0). Never set that pref.
Status and global configuration work without systemd.

LMS's mixer volume command implicitly unmutes; mute/unmute also complete a
server-side fade. Reverse control preserves the app's mute state and waits
400 ms for a mute transition before setting its new level, so a late fade
callback cannot undo that level. The real LMS volume test exercises this.

## Sonos static queue mirror

The retained WAV's frame zero corresponds to the known content position of its
from-start production. For a trimmed ALAC file this can be source frame 113.
An interior attach uses `landed_out - wavOrigin + warmup_out`.
Discard the converted producer's first `warmup_out` output frames, retaining the
from-start WAV's settled samples, then verify all 4096 overlap frames. The renderer and retained WAV use
WAV-relative coordinates; content positions use output coordinates for converted content.
Verify the full 4096-frame overlap before exposing an interior attach. Keep exact
PCM comparison, including ReplayGain. A mismatch creates a fresh producer item;
it does not mutate or cancel the retained WAV. Diagnostics include the first
differing WAV frame, landed source frame, WAV origin, and both wire gains.

| Source | LMS seek decision |
| --- | --- |
| Native unconverted ALAC, FLAC, PCM with known landing and available prefix | in-item |
| Converted hi-res to 44.1/48 kHz, factor-aligned known landing and known retained WAV origin and prefix covering warm-up plus 4096 frames | in-item after warm-up |
| Converted unknown/non-factor-aligned landing | fallback: landed-frame-unknown |
| AAC / MP3 with unprovable landing | fallback: landed-frame-unknown |
| Exact overlap mismatch | fallback: landed-pcm-mismatch |
| Unavailable verification bytes | fallback: verification-short-read |
| Prefix does not cover the landing and full overlap | fallback: prefix-unavailable |

Queue URLs are path only: `/yeney/q/<session>/<epoch>/<revision>/<item>.wav`, including lazy and continuation items. Session path segments are masked in logs/status. The old query route is removed because assets and sessions are process-local and cannot survive restart.

The queue mirrors the finite LMS playlist in order, with ordinal i mapped to
LMS index i-1, up to `YENEY_QUEUE_ROWS` (default 500). Configuration precedence
is environment, `room.<name>.YENEY_QUEUE_ROWS`, global `YENEY_QUEUE_ROWS`, default.
Every occurrence has a stable token and a lazy deferred full-length route.
DIDL uses LMS metadata and duration; size and sampleFrequency are omitted until
production. Continuous sources keep the stream path. Beyond the cap the legacy
window path remains and logs `window rebuild reason=beyond-cap`.

Build the mirror once per playlist generation. Enqueue the current item and
start playback before adding the rest in batches of at most 16 using
AddMultipleURIsToQueue. A definitive unsupported/invalid-action SOAP fault
selects AddURIToQueue fallback and is logged once. Each batch is verified with
Browse/UpdateID fencing. A generation change may rebuild the owned queue and
logs its edit count.

Ordinary LMS navigation uses one TRACK_NR Seek and Play only when needed;
app navigation uses the existing index-keyed arbiter and one playlist index
command. Self-originated TRACK_NR feedback is fenced with LMS index and
occurrence token. In-item seeks, speaker rewinds and natural transitions retain
the existing byte-verification and serialized outbox rules, I1-I6 below.
Navigation preserves queue rows. Deferred PCM requests activate through the
existing lazy/pairing path; HEAD and invalid/header-only probes do not select.

Retention is an audio cache policy: keep current and next three produced
parents protected, with the existing 256 MiB local / 8 GiB finite-remote quotas.
Played audio may be evicted without removing a row. A later selection reproduces
it through the lazy route with `decision=re-produced reason=evicted`; store
eviction is distinct from a source fault. Continuations retain parent identity.

Round 1 permits a seek fallback whose retained WAV cannot serve the landing to
replace that row through the existing fenced add/remove path. Log each use as
`window replace ordinal=N reason=seek-partial edits=2`. A partial cannot be
rebound under its immutable URI to a frame-zero full WAV: before history can
select it, publish a distinct deferred frame-zero history route. B1/B2/F3 retain
their PCM, command and switch checks. Round 2 removes this temporary exception
with sparse full-length items and defines beyond-cap policy. Later work adds
playlist diffs and SaveQueue takeover/restore. See
`docs/designs/sonos-full-queue.md` for measurements and acceptance status.

## Finite remote queue items

Read-only LMS `status` requests include duration (`d`) and live edge (`V`).
Local files retain their exact promises. A remote item with a finite positive
LMS duration and no live marker (`live_edge` absent or negative) selects relay
with `reason=remote-finite`; live/unknown-duration sources retain stream.
The remote promise preserves LMS's positive duration as a safe larger bound;
the pre-start status time can belong to the preceding run and is never subtracted.
Native LMS 9.1.1 podcast continue reports a full duration and absolute time;
keeping the full bound also handles a provider reporting remaining duration. `remoteMarginSeconds` is 5;
header, Content-Length and DIDL derive from the same declared frame count.

`PromiseKind::UpperBound` is distinct from exact local publication: short EOF
finishes normally without padding or LengthFault. Range at/beyond a short EOF
still announces the declared 206 range and closes with no body. Overflow is
spooled into ordered continuations with the same metadata, inserted after their
parent segments before the following LMS item. Publish an exact remainder as soon as the producer end resolves, or a generous upper bound at the ten-second prefetch deadline; log the remaining lead. Ordinary continuation transitions never produce
another LMS start and use parent-relative positions. Estimated episode segments
are retained together; default quota becomes 8 GiB, explicit quotas take priority.
No runtime feature depends on systemd.

A continuation uses the exact remaining frame count when the producer's end
resolves before the ten-second deadline. Existing published promises stay immutable;
unpublished overflow stays in the compressed spool until the end is resolved or ten seconds remain before the preceding segment ends. Exact completion publishes one exact remainder; the deadline publishes overflow produced so far plus the parent full duration, capped by room quota and WAV limits. Only exceeding that generous bound creates another segment.

Cached resume from a continuation matches the parent LMS producer, retains the
same queue assets, and reanchors the existing cached-start handshake to that
parent. It does not turn the continuation into another LMS track.

Permanent stdout diagnostics (default on) are `queue promise`, `queue HTTP`,
`queue continuation` and `queue end decision`. They omit source URLs and HTTP
session tokens. HTTP summaries include method, numeric Range, response length,
produced/served bytes and termination. TRANSITIONING is never completion.
Finite remote STMd waits for producer completion and the played episode frame
count, including continuations; local decode-ahead behavior is unchanged.
A native remote pause may flush LMS streaming while mode remains pause. Verify
same source and pause mode before retaining the queue/spool; explicit stop still
releases. The existing verified cached-start handshake handles resumed runs.

Finite remote encoded input uses an unlinked disk FIFO in the existing room
store, instead of the ordinary 256 KiB RAM FIFO. Its advertised capacity is real,
bounded by half the room quota and the 32-bit STAT limit; actual encoded file
bytes share quota accounting with PCM. HTTP reception continues during pause;
ordinary p/u resumes the original unfinished decoder. Local/stream input remains
the same RAM ring. A read-only clock acknowledgement precedes the finite remote
pause mutation so LMS saves the speaker's position. A verified native q+s cache
echo may temporarily expose new seek offset plus old-run STAT time; verify that
clock equation explicitly, without widening the existing seek tolerance.

Finite WAV HTTP sends distinguish receiver backpressure from EOF. Retry only
transient send errors, preserving the exact accepted-byte offset from the socket
counter; stop on peer close, cancellation or a real send error. Keep the existing
500 ms socket deadline. HTTP summaries include backpressure retry counts.

Native Podcast can store a player-formatted title while omitting artist/album
fields. For a cold formatted title, `podcast_title.h` resolves the canonical
title through the matching read-only podcast feed menu within a bounded metadata
request budget. Cached canonical titles are keyed by source URL, excluding native
bookmark markers. Tests use loopback LMS only; owner gateway reads are separate
manual diagnostics and must never be added to `make test`.

At the ten-second deadline, an unfinished producer gets its upper-bound fallback
even if no overflow is available yet. If the real source ends inside the parent
bound, the unused fallback closes at real EOF without padding, another LMS start
or a jump in the episode clock. Further items require an exceeded fallback bound.

### Podcast field matching (PODCAST-05)

The 30 October owner gateway returns plain America episode titles, so requiring
` by ` / ` from ` before browsing rejects them. The Plus menu decoration also
must not become the inferred podcast name. Match the known episode first; for
formatted LMS titles, keep its publisher and podcast text, using menu names
with ` | …` decorations removed to choose among delimiter splits. Names can
contain ` by ` and ` from `. Plain titles use the matched feed name; an absent
publisher stays empty. Multiple episode matches or splits remain unresolved.
The two-second total lookup budget is unchanged. Incomplete cached fields cause
a fresh lookup; canonical title alone is insufficient.

Send the `queue podcast metadata step=` journal/stdout line with the next
report. It names feed/episode matching, lookup timeout or CLI failure, cache hit
without podcast fields, or ambiguous split and candidate names. Include title,
feed menu name and settings; omit source URLs, tokens and descriptions.

## Navigation invariants

- I1: Every live Sonos PCM request has an owned producer selection. The room
  worker's serialized arbiter keys selections by LMS index; lazy activation
  and speaker navigation share it. Bind an already selected index, join its
  existing selection, or own exactly one selection command. Drain live WAV
  declarations in arrival order before advancing LMS's single producer.
  A selection for another index cannot silently discard a request.
- I2: Empty or short snapshots of the known playlist during navigation, restart or window edits are
  stale evidence. Reject them. Clearing requires a fresh empty read
  and no navigation or edit in flight; genuine user clear still stops.
- I3: A valid frame-zero target from the same source is always paired, including
  unfinished decode-ahead and previous selections of the same index. The fresh
  producer continues the existing immutable WAV and binds its clock to that URI.
  Re-production is only for audio that cannot serve the request (seek partial,
  source mismatch or unusable incomplete audio), and is produced before the
  single speaker switch. Seek-partial history uses a deferred frame-zero route;
  it never serves its old seek position on Back. Pairing failure must not stop
  the room or drop HTTP. Each navigation logs paired or re-produced with a reason.

- I4: A seek never replaces a queue row whose immutable WAV holds the seek point.
  The renderer seeks inside that WAV; the new LMS producer attaches at its true
  landed frame (content_frame), never at origin_ms. A partial item is produced
  only when the landed frame is unknown or the produced prefix does not reach
  it, and the reason is logged.
- I5: LMS keeps a seek start offset that STAT cannot rewind below. An
  app-originated position below that offset issues exactly one reason-checked
  `time` command; LMS's restarted producer attaches in-item and the renderer
  is never seeked or played again for that echo: decide by the same index-keyed
  Selection and its speaker-rewind reason, regardless of elapsed position.
  Every other app position change is zero-command. The 500 ms tolerance for
  other seek reconciliation remains unchanged.

- I6: The queue worker never terminates the process on data. Pairing, attach and
  verification failures are logged decisions that take the fallback path with
  one speaker switch; the HTTP reader of the retained item is retired by that
  switch, not cancelled by the failure. A genuine worker fault releases the
  room and stops it; it never aborts the process.

## Decisions — SEEK-01c (2026-11-11)

Stopped under the task's core-landing exception. Native ALAC from the private
M4A seek response reports source frame 0, but its first 4096 frames exactly
match original source frame 1822720. ReplayGain is 39424 on both productions.
The 96→48 bridge path falls back with `landed-frame-unknown`; no claim of
private-media in-item acceptance or worker exception safety is made. Core and
pin remain `9fba26f`. See [byte evidence and source-type observations](docs/research/seek-01c-landed-frame.md).
R1, I6 enforcement and the expanded gates remain pending after this explicit
stop condition; this commit is not a device fix.

## Decisions — SEEK-01d (2026-11-11)

Pin core a9ab672 without modifying it. Native ALAC tests explicitly configure
LMS native ALAC output; otherwise LMS can silently transcode ALAC to FLAC.
Keep the disposable tracks tagged as ALAC and provide native alcx/mp4 profiles;
assert every ALAC-variant wire start uses codec l. Load these own configuration
files through a configuration-only extension in the disposable instance cache;
LMS does not load them from prefsdir. Use the production 256 MiB quota for the
61-second ALAC windows. Generic MP4 classification
after decode-ahead otherwise selects a slow FLAC seek transcode.
Use disposable database ReplayGain (-4.4 dB, peak 1) and player mode 1.
Generated fixtures remain public and deterministic; private tracks are symlinked
and read in place. Run the loopback gates serially because they share ports.
A source length fault is recorded on its item and no longer rethrown by waitIdle.
Programming faults stop the storage worker and are observed by the room worker,
which releases the room through its existing roomFault path.

Rate-converted origins were already unknown on the baseline. Preserve observed
passing conversion baselines as regression evidence; do not manufacture a red
solely to satisfy a before/after label.

The ALAC soak checks known frame zero through the codec-independent
`queue production` diagnostic and advances its fake clock to one second before
the actual track end. This preserves the original 23-second stimulus for the
24-second WAV/FLAC tracks and supports the required 61-second ALAC fixtures.

## Decisions — SEEK-02 (2026-11-12)

The content-position callback forwards output landing and FIR settling bound
through QueuePathSink before its deferred boundary. Interior pairing requires
`landed_out - wavOrigin + warmup_out + 4096` produced frames. Settling PCM is
kept only until verification completes, so a mismatch fallback still has its
complete original seek production. STAT and committed producer frame budgets
subtract the landing offset before settling, not the later verification offset.
The renderer still seeks to the requested WAV time.

Rewind authority is the existing index-keyed Selection with matching occurrence
token and rewind target. No persistent boolean guard is added. An unchanged
verified retained row needs no topology read at producer admission, no admission
Browse and no WindowEdits Browse chain; actual edits retain their fences.
The fake advances from a monotonic clock while PLAYING for navigation gates,
freezes while paused, and adds 600 ms per relevant SOAP response during rewinds.
Public generated fixtures cover native 44.1 plus four conversion pairs in both
codecs; private media is read in place. Owner devices are never contacted.

With the advancing fake, natural-boundary stimuli wait for the current producer
to finish, move within the normal prefetch lead, and prepare the next producer
before moving to the last second.
The fake keeps advancing throughout; the existing readiness deadlines remain
unchanged. Moving first could carry it beyond EOF during the readiness wait.

Interior attachment also requires the original retained WAV's known content
origin. Follow the existing pairedFrom chain; a later frame-zero alias with a
known producer landing cannot establish an unknown original WAV coordinate.
Use landed-frame-unknown fallback when that coordinate cannot be proved.

## Decisions — FULLQ-01 resolved by FULLQ-01b (2026-11-12)

The owner permits the existing single-row seek-partial replacement and deferred
frame-zero history route for round 1. This resolves the representation stop
recorded in [FULLQ-01](docs/research/fullq-01-stop.md). Restore the two reverted
model/batch commits and saved experiment; preserve immutable WAV promises and
B1/B2/F3. Core and pin stay `9d5aa42`. Sparse full-length items remain round 2.


## Decisions — FULLQ-01c acceptance status

The acceptance fake uses the device profile: 30 ms SOAP, 400 ms transport
transition and 50 ms GENA echo, calibrated from the owner's 12 November Study
journal. The 600 ms profile is an optional stress test, not device acceptance.

Static-mirror SOAP contract, measured from the trigger to first audible PCM:
- LMS Next, Previous, jump and repeat wrap: one `Seek TRACK_NR`; add `Play`
  only when playback is not already running. No Browse, topology read or edits.
- App Next, Back and row tap: no SOAP writes; exactly one LMS `playlist index`.
- In-item LMS seek: one `Seek REL_TIME`, no queue edits.
- Cold ownership: one fresh topology read, reused within the owned sequence;
  GENA group changes revoke permission before writes. Backup verification and
  real-edit fences remain. Backup retention runs after Play, outside admission.

Q8 is resolved: a partial seek followed by Next swaps the same row exactly
twice, full → suffix → frame-zero history, each with
`window replace … reason=seek-partial edits=2`. No other row replacements are
allowed. This is the only navigation path with row edits until sparse items
land. B1/B2/F3 and immutable WAV resources remain unchanged.

FLAC passes the device timing matrix. Tail-moov ALAC and raw WAV still miss
latency deadlines with core pin `9d5aa42`; the assertions remain enabled.
Faststart ALAC removes the metadata-at-EOF delay without changing PCM. The
device-profile Q4 quota case also reproduces a baseline pressure/navigation
race; it remains outstanding without a mirror/outbox model change. See
`docs/designs/sonos-full-queue.md` for measured numbers and final gate evidence.

Incoming finite starts carry an occurrence-token/producer-lower-bound admission
ticket before entering the asynchronous core path. The serialized app-selection outbox
waits for that same occurrence's codec declaration before its one index command;
`latestOccurrence()` alone can still describe the preceding producer at this
point. Keep topology/edit fences, immutable promises and byte checks intact.

A controller move may retire an older app selection only when its retained WAV
already supplies its complete promise (or a finished upper-bound resource).
Keep incomplete declarations protected. Generation-rebuild edit counts are
logged after both removal and row publication complete.
