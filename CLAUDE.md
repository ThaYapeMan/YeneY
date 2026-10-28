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

## Finite remote queue items

Read-only LMS `status` requests include duration (`d`) and live edge (`V`).
Local files retain their exact promises. A remote item with a finite positive
LMS duration and no live marker (`live_edge` absent or negative) selects relay
with `reason=remote-finite`; live/unknown-duration sources retain stream.
Remaining duration excludes LMS's resume offset. `remoteMarginSeconds` is 5;
header, Content-Length and DIDL derive from the same declared frame count.

`PromiseKind::UpperBound` is distinct from exact local publication: short EOF
finishes normally without padding or LengthFault. Range at/beyond a short EOF
still announces the declared 206 range and closes with no body. Overflow is
spooled into ordered continuations with the same metadata, inserted after their
parent segments before the following LMS item. Insert immediately; warn when
less than ten seconds remain before their boundary. Ordinary continuation transitions never produce
another LMS start and use parent-relative positions. Estimated episode segments
are retained together; default quota becomes 8 GiB, explicit quotas take priority.
No runtime feature depends on systemd.

A newly created continuation uses the exact remaining frame count when the
producer has already resolved its end. Existing published promises stay immutable;
continuations created while the end is unknown keep the five-second upper bound.

Cached resume from a continuation matches the parent LMS producer, retains the
same queue assets, and reanchors the existing cached-start handshake to that
parent. It does not turn the continuation into another LMS track.
