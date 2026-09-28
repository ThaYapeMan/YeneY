# CLAUDE.md

## What this is

Registers Sonos speakers as LMS players; synchronised playback with other LMS players is not yet accurate. Not an LMS plugin — a standalone program built on the squeezelite codebase with a Sonos output driver instead of ALSA.

## Build environment

The WSL2 checkout at `/mnt/c/HueSyncLXC/sonos-lms` is the development environment. Build locally with `make` and run automated tests with `make test`. The Makefile supplies the compatibility flags for current compilers.

LXC 113 (192.168.178.31) is deployment only. Agents must never SSH to it or deploy there. After the agent commits and pushes, the user runs `git pull && git submodule update --init --recursive && make && sudo make install` on the deployment target. The installer restarts the configured room services listed in `/etc/sonos-lms/rooms`.

## Submodules

- `squeezelite/` is our fork: `ThaYapeMan/squeezelite`. Changes are allowed. Commit them in the fork repository as normal commits on its default branch and push there first; then bump the submodule pin in `sonos-lms` in a separate commit.
- Never rewrite history on a fork's published branch: no rebase, amend, or force-push. `ThaYapeMan/squeezelite` is also pinned by LampaStream in `scripts/build-squeezelite.sh`. The author-identity rewrite on 2026-09-23 (`sonos-lms` commit `30c83f5`, squeezelite `9a34622` -> `0e1667e`) orphaned LampaStream's pin and broke its fresh installs. Before any squeezelite pin change, check LampaStream's pin and ensure its pinned commit remains fetchable from the fork.
- Never leave edits inside a submodule checkout without committing them to the fork. `git submodule update` is not a way to preserve local work; forced updates discard uncommitted changes. Commit and push fork changes before updating pins or checkouts.

## UPnP layer

YeneY is the only backend. `SONOS_LMS_UPNP=yeney` and `own` are accepted; other values warn and continue with YeneY. Keep HTTP Server constants, wire fixtures, URLs, transport ordering and timing unchanged unless the task explicitly requires a change. The encoded buffer capacity is in packets, not bytes.

## Our modifications

- **Track metadata**: LMS CLI (port 9090) is queried for title, artist, album and artwork at stream start. `SONOS_LMS_TITLE_FORMAT=artist-title|title` formats the sent title (default artist-title); keep the log and cached display title consistent with DIDL. `scripts/meta-variant.py` is an owner-run device diagnostic.
- **MAC address**: sent raw, not URL-encoded. URL-encoding causes LMS to silently fail to match the player.
- **Artwork URL**: constructed as `http://<lms>:9000/music/<id>/cover.jpg` because LMS does not include `artwork_url` for local tracks.
- **Player name**: registered as `<room> (Sonos)` (e.g. `Study (Sonos)`) — human-readable in every LMS controller.

## Rejected approaches

**ICY/Shoutcast in-band metadata** — Sonos never sends the `Icy-MetaData: 1` opt-in header for `audio/flac` streams, so injecting ICY blocks corrupts the stream. See README for details.

## Diagnostics

The program line-buffers stdout, including under systemd. Use `journalctl` for transport/stream logs and `ss -tnp` for connection state. See `DEVICE-VERIFICATION.md` for the physical-device acceptance checks.

## Pitfalls

- Never pass `--server=<ip>:9000`; port 9000 is the web interface. Pass `--server=<ip>` only — slimproto is found on port 3483.

## Conventions

- All documentation and commit messages in English.
- Always push after committing.
