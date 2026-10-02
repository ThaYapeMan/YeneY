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

- **Track metadata**: LMS CLI (port 9090) is queried for title, artist, album and artwork at stream start. `YENEY_TITLE_FORMAT=artist-title|title` formats the sent title (default artist-title); keep the log and cached display title consistent with DIDL. `scripts/meta-variant.py` is an owner-run device diagnostic.
- **MAC address**: sent raw, not URL-encoded. URL-encoding causes LMS to silently fail to match the player.
- **Artwork URL**: constructed as `http://<lms>:9000/music/<id>/cover.jpg` because LMS does not include `artwork_url` for local tracks.
- **Player name**: registered as `<room> (Sonos)` (e.g. `Study (Sonos)`) — human-readable in every LMS controller.

## Rejected approaches

**ICY/Shoutcast in-band metadata** — 28 September device probes show that the CurrentURI scheme selects the client: `http://` does not request ICY and plays FLAC; `x-rincon-mp3radio://` requests ICY for MP3 and FLAC but cannot play FLAC. Keep the bridge's HTTP CurrentURI with radio `audio/flac` protocolInfo. MP3 ICY StreamTitle reaches streamContent, but the current app still shows only the title. See README for details.

## Diagnostics

The program line-buffers stdout, including under systemd. Use `journalctl` for transport/stream logs and `ss -tnp` for connection state. See `DEVICE-VERIFICATION.md` for the physical-device acceptance checks.

## Pitfalls

- Never pass `--server=<ip>:9000`; port 9000 is the web interface. Pass `--server=<ip>` only — slimproto is found on port 3483.

## Conventions

- All documentation and commit messages in English.
- Always push after committing.
