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

YeneY is the only backend. `YENEY_UPNP=yeney` and `own` are accepted; other values warn and continue with YeneY. Keep HTTP Server constants, wire fixtures, URLs, transport ordering and timing unchanged unless the task explicitly requires a change. The encoded buffer capacity is in packets, not bytes.

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

## Sonos queue window

The window model has two history parents, the current parent and three upcoming
parents. Continuations share their parent's history slot. The production relay
now retains two played parents and trims older parent blocks through
`WindowEdits`, with fresh Browse/UpdateID fencing and the audible parent as
anchor. It does not issue an LMS command when sliding history. The natural
producer and its WAV declaration remain unchanged.

The sink protects retained history and all its continuation audio from quota
eviction. Parents removed by the worker release RAM and compressed files
immediately; existing readers fail rather than receiving invented audio. Default
local/finite-remote quotas remain 256 MiB/8 GiB, with explicit quotas authoritative.
Permanent `queue window remove ordinal=... count=... lms_indices=...
reason=history-limit` stdout lines describe trims without source URLs or tokens.

Lazy HTTP declarations and the six-parent playlist model are still building
blocks: the production relay does not yet publish lazy upcoming entries or
reconcile LMS navigation/playlist edits against that model. Lazy activation,
binding and abandonment diagnostics are therefore not production features yet.
Stage 2 app navigation/seek commands remain unimplemented. See
`docs/designs/sonos-queue-window.md` for the integration checkpoint. All current
retention and diagnostics work without systemd.

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
