# YeneY — play Lyrion Music Server (LMS) on Sonos

**YeneY turns every Sonos speaker into a real Lyrion Music Server player.** Lyrion
Music Server (LMS) was formerly called Logitech Media Server or Squeezebox Server.
Your Sonos rooms appear in LMS next to your Squeezebox players, with their own Sonos
icon in Material Skin. You control them from any LMS app: Material Skin, the web
interface, iPeng or Squeezer. LMS stays in charge of the playlist and the sound.

**What YeneY does better than other LMS-to-Sonos bridges:**

- **Sound quality.** Lossless 24-bit audio. At 44.1 and 48 kHz the samples LMS
  produces reach the Sonos unchanged (bit-perfect when ReplayGain is off; YeneY
  never applies LMS volume to the audio). Hi-res files (88.2–192 kHz) are converted
  to the Sonos maximum by YeneY's own converter, measured equal to SoX at its
  highest quality setting (`rate -v`): passband ripple below ±0.0002 dB and
  aliasing below −120 dBFS ([measurements](docs/designs/queue-hires-decimation.md)).
- **Track details.** In queue mode every LMS track is its own item in the Sonos
  queue, with its own title, artist, album, artwork and duration in the Sonos app.
  Podcast and radio artwork comes through LMS's image proxy. Damaged tags are
  cleaned so they can never stop playback.
- **Fast starts.** Local tracks start in about 1.3 s, hi-res tracks in under
  0.5 s, podcasts and radio in under a second.
- **Sonos-app pause and stop reach LMS.** Pause, resume and stop on the Sonos side
  are passed back to LMS, without echo loops.
- **Integration with LMS.** YeneY behaves like a well-mannered squeezelite player:
  LMS's playlist index stays in step with what the speaker plays, YeneY sets its
  own player icon, and it never sends LMS commands in reaction to LMS's own
  messages.

No Sonos-specific app, no pairing step per track, and no cloud service: YeneY runs
on your own network, on any small Linux machine, container or VM.

**YeneY** is pronounced **YEN-ee**. The palindrome nods to *Sonos*.

## Two playback modes

| Mode | How it plays | Best for |
|---|---|---|
| **Stream** (default) | One continuous 24-bit FLAC stream per room, paced on the Sonos clock | Grouped rooms; everything LMS can play |
| **Queue** (per-room opt-in, `YENEY_PLAYBACK_MODE=queue`) | Local files: one Sonos queue item per LMS track (exact-length 24-bit WAV, hi-res converted by YeneY). Podcasts, radio and streaming services switch to the stream path automatically | Standalone rooms: per-track titles and artwork, fast starts, hi-res |

Queue mode was rebuilt in October 2026 as a small relay that follows LMS strictly,
in the spirit of philippe44's LMS-uPnP. The earlier, larger design is preserved at
the `queue-full-v1` tag; its features return one at a time on top of the relay
(see the [rebuild checklist](docs/designs/queue-parked-features.md)).

## YeneY compared with LMS-uPnP

[philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP) (UPnPBridge) is the
mature, general-purpose bridge between LMS and UPnP/DLNA speakers, Sonos being one
of many. It has two modes: *per track* (the speaker plays one track at a time) and
*flow* (one continuous stream). It works with almost any UPnP renderer and has
years of use behind it. YeneY is built for Sonos only, and for Sonos it does
clearly more in **sound quality, track details, start times, Sonos-app pause and
LMS integration**:

| | YeneY | LMS-uPnP, per track | LMS-uPnP, flow mode |
|---|---|---|---|
| Lossless, 24-bit | ✔ 24-bit throughout; bit-perfect at 44.1/48 kHz with ReplayGain off | ✔ FLAC for files; radio passed through as is | ~ lossless, but changing titles need MP3/AAC |
| Hi-res sources (88.2–192 kHz) | ✔ converted by YeneY's own converter, measured equal to SoX `rate -v`; start under 0.5 s (queue mode) | ~ depends on LMS transcoding and the speaker | ~ resampled to the flow rate |
| LMS sound processing | ✔ ReplayGain, crossfade, DSP | ~ ReplayGain and resampling, no crossfade (none at all in pass-through mode) | ✔ ReplayGain, resampling and crossfade |
| Gapless | ✔ stream mode: one continuous stream; queue mode: exact-length items, sample-continuous joins | ~ via SetNextAVTransportURI; depends on the speaker | ✔ gapless and crossfade |
| Title, artist, album and artwork per track in the Sonos app | ✔ queue mode, including podcast artwork; stream mode: at the start of each stream | ✔ per track | ~ only with MP3/AAC (ICY) |
| Start time | ✔ local ~1.3 s, hi-res < 0.5 s, podcasts/radio < 1 s | ? not measured | ? not measured |
| Pause/stop in the Sonos app reaches LMS | ✔ pause, resume and stop, without echo loops | ✔ | ~ Sonos sees a live stream; pause needs a workaround (`live_pause`) |
| Damaged tags (control characters, invalid UTF-8) | ✔ cleaned; playback never fails on them | ? not verified | ? not verified |
| Own player icon in Material Skin | ✔ set automatically | ✔ DLNA icon | ✔ DLNA icon |
| Next/Previous and seeking in the Sonos app | ✘ not yet (from LMS: ✔) | ? not verified | ✘ live stream |
| Grouped Sonos rooms | ~ stream mode only; queue mode for standalone rooms | ✔ | ✔ |
| Synchronises with other LMS players | ✘ not yet | ? not verified | ? not verified |
| Other UPnP/DLNA speakers | ✘ Sonos only | ✔ | ✔ |
| Maturity | new; tested daily on two Play:1 speakers and a Sonos Port | mature, years of use on many devices | mature |

✔ yes · ✘ no · ~ partly · ? not verified yet

### Which one should I use?

| Choose YeneY if you… | Choose LMS-uPnP if you… |
|---|---|
| want the best sound on Sonos: 24-bit, bit-perfect at 44.1/48 kHz, top-quality hi-res conversion | have speakers from other brands as well |
| want correct titles and artwork per track, and fast starts | want Next/Previous and seeking in the Sonos app today |
| control playback from LMS (Material Skin, the web interface, iPeng) and want Sonos-app pause and stop to reach LMS | group Sonos rooms a lot |
| only have Sonos speakers | prefer a long-proven solution |

**Why YeneY exists.** LMS-uPnP is a general bridge: it serves many kinds of
renderers well, and Sonos is one of them. YeneY is made only for Sonos and LMS, and
uses that focus where it matters most: the sound (24-bit, bit-perfect at
44.1/48 kHz, hi-res conversion as good as SoX's best), the track details in the
Sonos app, start times, pause and stop from the Sonos app, and a clean integration
with LMS. It also solves a Sonos limit that affects any continuous stream: a Sonos
cannot resume a paused radio-style FLAC stream without an error. Background:
[Why a continuous stream](docs/how-it-works.md#why-a-continuous-stream-and-how-this-differs-from-track-by-track-upnp)
and [Sonos queue mode](docs/designs/sonos-queue-mode.md).

## What you need

- **Sonos speakers on the S2 software.** Tested on a Play:1 (firmware 86.10) and a
  Sonos Port (97.2). S1-only speakers have not been tested.
- **Lyrion Music Server** on your network (formerly Logitech Media Server).
- **A Linux machine for YeneY**, for example a Debian or Ubuntu container or VM.
  It needs to reach the Sonos speakers and LMS on the local network. One small
  process runs per Sonos room.

## How it works in one paragraph

For every Sonos room, YeneY registers a player with LMS, exactly like a Squeezebox
would. LMS sends it the music; YeneY encodes it as one continuous FLAC stream and
tells the Sonos speaker to play that stream, while keeping play, pause, stop,
volume, position and track information in step on both sides. LMS stays in charge
of the playlist and the sound; the Sonos simply plays what it receives. In queue
mode, local tracks are instead published one by one as exact-length items in the
Sonos queue, while podcasts and radio keep using the stream. The full
story, including the Sonos pause problem YeneY solves, is in
[How YeneY works](docs/how-it-works.md).

## Installing

The commands use `sudo`; in a container where you are already root, you can leave it out.

Install the build tools and libraries (Debian/Ubuntu):

```sh
sudo apt-get install -y --no-install-recommends \
    git make g++ python3 libssl-dev libflac++-dev libflac-dev
```

Get the source and build it:

```sh
sudo git clone --recursive https://github.com/ThaYapeMan/YeneY.git /opt/yeney
cd /opt/yeney
sudo make
```

Install a service for each Sonos room:

```sh
sudo make install
```

The installer finds LMS and your Sonos rooms, asks which rooms to bridge, and
starts one `yeney@<room>` service per room. Each room then appears in LMS as
`<room> (Sonos)`. Follow the logs with:

```sh
journalctl -u 'yeney@*' -f
```

### Updating

```sh
cd /opt/yeney
sudo git pull
sudo git submodule update --init --recursive
sudo make
sudo make install
```

The installer keeps your configuration. It asks `Keep this configuration? [Y/n]`
and restarts only the rooms that run an older build.

### Installer details

On the first install, it asks for the LMS server and every discovered room. When
an existing config has an `LMS_SERVER` line and at least one room, it shows the
saved configuration and asks `Keep this configuration? [Y/n]`. Enter or `y` keeps
it, adds newly discovered rooms as `no`, and displays and applies the plan without
another confirmation. Answer `n` to configure the LMS server and every discovered
room, then review the diff and confirm `Apply? [Y/n]`. `--reconfigure` goes straight
to that full configuration flow. Offline rooms retain their config lines and
appear as offline. Room arguments are enabled without per-room questions.

Example re-run after a new build and discovery of MBR:

```text
YeneY installer — build abc1234
LMS server: 192.0.2.23 (config)
Sonos rooms found: 3
Room        Model   IP          Group                          Bridge to LMS
MBR         One     192.0.2.24  -                              new, not bridged
Sonos Port  Port    192.0.2.25  member of Study                yes, running
Study       Play:1  192.0.2.26  coordinator: Study+Sonos Port  yes, running
Keep this configuration? [Y/n]
Config:
--- current config
+++ proposed config
@@ -4,2 +4,3 @@
 room.Sonos Port=yes
 room.Study=yes
+room.MBR=no
Restart: Sonos Port, Study (new build abc1234); playback stops briefly
Build record: update installed-build
Sonos Port  running   LMS player "Sonos Port (Sonos)" connected
Study       running   LMS player "Study (Sonos)" connected
MBR         disabled
Logs: journalctl -u 'yeney@*' -f
```

With the same build and settings, the plan instead includes:

```text
Keep this configuration? [Y/n]
Config:  no changes
Keep:    Sonos Port, Study
Nothing to do.
```

Kept rooms are **not restarted**. Every install restarts all enabled room services
when the binary changes, including rooms already running. A changed service
template or installer-managed drop-in also restarts all enabled rooms, after
`systemctl daemon-reload`. Unchanged builds and configuration may be kept; an LMS
setting changed in this run or `--restart` also requests a restart. Each restarted
room is reported as `restarted` alongside its LMS player check. An interrupted
unit update remains pending until its service actions succeed.
`/etc/yeney/installed-build` records the commit (with `-dirty` when applicable)
and binary SHA-256; it is written atomically only after service actions succeed.
The plan lists Start, Restart (with its reason), Stop and Keep, plus any service
enabling, template installation, migration or build-record changes. With no work,
there is no Apply prompt. Declining Apply or pressing Ctrl-C at a prompt changes
no files or services.

Afterward, enabled rooms (including kept rooms) share an LMS CLI check on port
9090, waiting at most 15 seconds for `<room> (Sonos)` with `connected:1`.
A missing host or unreachable CLI skips the check without failing installation;
a running service alone does not prove it reached LMS.

Alternatively, edit `/etc/yeney/config` and run
`sudo scripts/install-devices.sh --non-interactive` (or `sudo make install` and
accept the defaults). Example config:

```ini
# LMS host or IP (no port). Empty = automatic discovery on the local network.
LMS_SERVER=192.0.2.23
# Sonos rooms found on the network.
# yes = bridge this room to LMS, no = ignore it.
room.Sonos Port=yes
room.Study=yes
room.MBR=yes
```

Set a room to `no` and apply to stop/disable its bridge. Existing comments, ordering
and unrelated settings are preserved. The old `rooms` file migrates once to
`room.<name>=yes` settings and is renamed `rooms.migrated` after approval.

Installer flags (`scripts/install-devices.sh`):

| Option | Behaviour |
| --- | --- |
| `--yes` | Apply the displayed plan without prompts; new rooms default to `no`, existing values stay unchanged. |
| `--non-interactive` | Same as `--yes`; automatic when stdin or stdout is not a TTY. |
| `--reconfigure` | Skip the keep question; ask for the LMS server and every discovered room on a TTY. Non-interactive flags take precedence. |
| `--restart` | Force restart of every running enabled room; plan reason is `forced`. Stopped enabled rooms are started. |
| `--server=<host>` | Override the saved LMS host (no port, whitespace or control characters). |

Quoted room arguments such as `sudo scripts/install-devices.sh "Sonos Port"`
set those rooms to `yes`. Interactive declines produce no new-room reminder;
non-interactive runs print one combined reminder for newly added disabled rooms.

## Settings

Settings are environment variables per room, set with a systemd override. For
example, for the room Study:

```sh
sudo systemctl edit 'yeney@Study'
```

Add the setting under `[Service]`, then restart the room:

```ini
[Service]
Environment=YENEY_TITLE_FORMAT=title
```

```sh
sudo systemctl restart 'yeney@Study'
```

Settings are read once; timing RAW/STALE are inspected only when the probe is enabled. An invalid value logs a warning
and uses the default, except for an invalid queue disk quota, which fails queue configuration.

| Setting | Values | Default |
|---|---|---|
| `YENEY_PLAYBACK_MODE` | `stream`: existing transport · `queue`: standalone owned queue opt-in · `auto`: stream, logged as `auto-phase4` | `stream` |
| `YENEY_QUEUE_POLL_MS` | Queue steady speaker poll interval, 100–2000 ms; starts and joins use 100 ms windows | `500` |
| `YENEY_QUEUE_NEXT_DELAY_S` | 1–120 seconds: request the remote successor only below this remaining audible time, after full production | `20` |
| `YENEY_QUEUE_DISK_MIB` | Positive disk-history quota in MiB per queue room; invalid/overflowing values fail queue configuration | `256` |
| `YENEY_LMS_ICON` | Material icon name; explicitly empty disables preference handling | `sonos` |
| `YENEY_START_LEAD_MS` | Startup reserve, 0–10000 milliseconds | `1000` |
| `YENEY_UPNP` | `yeney` or `own`; unsupported names warn and use YeneY | `yeney` |
| `YENEY_STREAM_PACING` | `sonos`: follow the Sonos clock · `wall`: legacy host clock opt-out | `sonos` |
| `YENEY_AUDIO` | `24/48`: 24-bit FLAC at the source's 44.1/48 kHz rate · `16/44`: legacy 16-bit/44.1 kHz | `24/48` |
| `YENEY_TITLE_FORMAT` | `artist-title`: "artist - title" · `title`: title only | `artist-title` |
| `YENEY_STREAM_CONTENT` | `structured`, `plain` or `off`: the radio-text field for other controllers | `structured` |
| `YENEY_PAUSE` | `stop`: the Sonos-safe pause · `pause`: plain UPnP Pause (fallback) | `stop` |
| `YENEY_DEBUG` | Comma-separated `events`, `stream`, `legacy-poll`, `stopped-mediainfo`; diagnostics and polling experiments only | empty |
| `YENEY_TIMING_PROBE` | `1`: measurement-only coordinator playback-clock probe; unset/`0`: off | off |
| `YENEY_TIMING_RAW` | `1`: per-request and edge observations, only with TIMING_PROBE enabled | off |
| `YENEY_TIMING_STALE_S` | 10–600 seconds without a usable edge before probe expiry | `60` |
| `YENEY_TIMING_LOCKED_EVERY_S` | 1–60 seconds between locked clock probes | `5` |
| `YENEY_LMS_POSITION_FROM_MODEL` | `1`: continuous locked-clock LMS position; `0`: RelTime-only. Requires an active timing probe model. | 1 |
| `YENEY_TIMING_PRIOR` | Use the persisted per-UDN drift prior | `1` |
| `YENEY_TIMING_PUBLISH` | `1`: publish the LampaStream timing contract and core audio tap, only with TIMING_PROBE | off |
| `YENEY_AUDIBLE_OFFSET_MS` | -500–500 ms acoustic calibration, added to published audible times | `0` |

Artwork in both stream and queue mode comes from LMS. Local library tracks keep
`http://<lms>:9000/music/<id>/cover.jpg`. Remote tracks (podcasts, internet radio
and streaming services) use LMS's `artwork_url`: relative paths use the LMS HTTP
host, and absolute HTTP/HTTPS images go through its image proxy so Sonos fetches
plain HTTP from LMS. Existing image-proxy paths retain their inner percent
escapes. Remote tracks without artwork send no album-art element. Stream
metadata is fetched at stream start; this adds no midstream polling.

Queue mode is an explicit per-room opt-in in the same systemd override:

```ini
[Service]
Environment=YENEY_PLAYBACK_MODE=queue
Environment=YENEY_QUEUE_DISK_MIB=256
```

Settings apply on room service restart. A grouped coordinator or member keeps
stream mode with reason `group-rate-undecided`; an unconfirmed coordinator uses
`coordinator-unconfirmed`. `auto` still uses stream. Queue mode owns only its
prepared finite WAV entries for local files. At each LMS track start, remote
flags, negative ids and non-file URLs (including podcasts, radio and streaming
services) select the existing continuous stream path instead; unknown metadata
also selects stream. Remote playback starts progressively without waiting for
a finite download and does not acquire the Sonos queue. Local/remote class
changes wait for the outgoing track to finish, then start the other path; a
short measured gap is logged. Room diagnostics expose `path=relay|stream` and
`reason=local-source|remote-source`. Pacing logs and socket-wait accumulation
are suppressed on the relay path and unchanged on the stream path. LMS controls play, pause, resume, stop and source
seeks; confirmed speaker starts advance LMS through SlimProto. Queue mode reports unambiguous speaker-originated stops and pause/resume to LMS
through one logged CLI mutation path. LMS-originated commands never cause a CLI
mutation. Source changes release owned entries without stopping the new source.
There is no automatic fallback in this relay release. Failures stop/release the owned entries and report STMn.
Whole-list mirroring, app next/previous/seek mapping, fallback and group/auto policy are parked at
`queue-full-v1`; see the [rebuild checklist](docs/designs/queue-parked-features.md).
Owners can qualify a standalone room with
`/usr/local/bin/yeney-queue-device-suite --room Study --server 192.168.178.23`,
optionally `--album <id-or-name> --volume 10`. The suite saves/restores volume,
LMS state and room configuration; it checks album joins, LMS pause/resume and
stop/new play, and a remote favourite/podcast when available, followed by a
local album returning to the relay. Missing remote entries produce a documented
skip. Use the [systemd-run procedure](DEVICE-VERIFICATION.md#phase-3c-unattended-queue-playback-device-suite)
to survive an SSH disconnect. [Relay status and limitations](docs/designs/sonos-queue-mode.md)
are documented separately. The relay passed the owner device suite on Study and
on the Sonos Port (21–23 October 2026), including LMS pause/resume, stop/new play,
external stops, remote sources and 88.2/176.4 and 96/192 kHz tracks.

On 19 October 2026, the owner qualified Study on bridge `5a686ad` with
`/usr/local/bin/yeney-stream-pacing-suite --room Study --hours 2`: **PACING OK**.
The run collected 720 samples at 10-second intervals over one connection, with
719/720 samples locked, zero overwrites and zero drops. Qualified lead was
1406–1499 ms, with a 1452 ms median in both hours; measured drift was about
11.5 ppm. This result supports making Sonos-clock pacing the default.

To opt out for a room, add `Environment=YENEY_STREAM_PACING=wall` under
`[Service]` in that room's systemd override (for example,
`sudo systemctl edit 'yeney@Study'`), then restart that room. Invalid values
warn and use `sonos`. Other rooms can be qualified with
`/usr/local/bin/yeney-stream-pacing-suite --room <room> --hours 2`; see
[device verification](DEVICE-VERIFICATION.md#unattended-owner-stream-pacing-qualification).

The details of each setting are in [How YeneY works](docs/how-it-works.md).

### LMS player icon (Material Skin)

The supplied Sonos logo is bundled in `assets/sonos.svg`. It is included in
YeneY's installation assets; you do not need to create or obtain an SVG.
Material looks for custom icons in `<LMS prefs dir>/material-skin/images`.
Find that preferences directory in **LMS Settings → Information**.

When LMS and YeneY share a host, configure the directory explicitly on the
first installation:

```sh
sudo make install LMS_PREFS_DIR=/absolute/path/to/lms/preferences
```

The installer installs `sonos.svg` there and remembers the directory in
`/etc/yeney/lms-prefs-dir` for later `sudo make install` runs. It does not guess
an LMS directory. If none is configured, installation clearly reports that the
Material asset was not installed; the bundled asset and command are still
installed under `/usr/local/share/yeney` and `/usr/local/bin`.

For LMS on a separate host, run the bundled asset installer **locally on the
LMS host**. From a YeneY checkout there, no build or room-service installation
is needed:

```sh
sudo /opt/yeney/scripts/yeney-install-lms-icon --lms-prefs-dir /absolute/path/to/lms/preferences
```

If YeneY's installation assets are already installed on that host, the equivalent
command is:

```sh
sudo /usr/local/bin/yeney-install-lms-icon --lms-prefs-dir /absolute/path/to/lms/preferences
```

Both commands use the bundled logo. The runtime does not access the LMS host's
filesystem. Repeated installation is idempotent, and updates to YeneY-managed
copies use atomic replacement. A different owner-managed `sonos.svg` causes a
clear conflict and is preserved. An identical pre-existing owner copy is left
in place without being claimed as YeneY-managed.

`YENEY_LMS_ICON` is a per-room systemd setting:

- Unset defaults to `sonos`, selecting the bundled logo.
- `Environment=YENEY_LMS_ICON=` disables automatic preference handling.
- A nonempty name matching `[A-Za-z0-9_-]{1,64}` selects an icon already
  available in Material Skin. Invalid values warn and disable handling.

Use `sudo systemctl edit 'yeney@Study'`, put the setting under `[Service]`,
and restart that room's service. Once per registered LMS connection, YeneY
checks `plugin.material-skin:icon` asynchronously. Empty preferences are set;
a value previously written and recorded by YeneY can be updated. Owner-selected
icons are preserved, and already-correct values are not rewritten or claimed.
Disabling handling leaves an existing icon in place. Timeouts and malformed
replies are logged without blocking or failing playback.

The installer generates a per-room `zz-yeney-state.conf` systemd drop-in. It
supplies `STATE_DIRECTORY` as `/var/lib/yeney/<slug>-<hash>`: an ASCII lowercase
slug (at most 40 characters) and the first 16 hexadecimal SHA-256 digits of the
original UTF-8 room name. Spaces, quotes and Unicode names are safe, and names
that produce the same slug remain distinct. Confirmed icon ownership and queue
state live there. `ExecStart` and the room argument remain unchanged. View a room's
configured path with `systemctl show -p StateDirectory 'yeney@Study.service'`.

In both stream and queue mode, the room's `stream-pacing-<UTF-8-room-hex>.json`
status file and drift-prior persistence use this directory. Only manual runs
without `STATE_DIRECTORY` use `/var/lib/yeney`. Queue storage and confirmed icon
ownership already follow the same room directory.

`/usr/local/bin/yeney-queue-device-suite` and
`/usr/local/bin/yeney-stream-pacing-suite` discover it automatically through
`systemd-escape` and `systemctl show … -p StateDirectory --value`, using their
bundled shared `yeney_room_state.py` helper. No copied files, symlinks or path
flags are needed. Failed or ambiguous systemd lookups are errors; only an empty
StateDirectory property selects the legacy directory. Installation removes old
status files for migrated rooms and global symlinks pointing into their state
directories, leaving other files alone.

The owner-tool audit also covers `yeney-lms-position-check` (journal and LMS CLI),
`yeney-timing-read` (the unchanged `/dev/shm` timing/audio objects), and
`yeney-queue-suite` (its isolated `/tmp` workspace). Those tools do not read the
room's persistent status/state files, so their data sources remain unchanged.

On install, existing state from escaped or raw room-name directories is moved
while that room is stopped. The temporary `/var/lib/yeney/sonos-port` workaround
is migrated too. The installer removes the Port's `statedir.conf` automatically
only if it contains exactly the three-line workaround (`[Service]`, empty
`StateDirectory=`, then `StateDirectory=yeney/sonos-port`). Custom contents or
symlinks are left in place with a warning. Identical state files can be merged;
conflicting copies stop installation without overwriting either copy. Ad-hoc runs without a state directory
skip automatic handling rather than writing an unrecorded preference.

The SVG has a 24×24 viewBox, monochrome `#000` fills, outlined paths and
`fill-rule="evenodd"` cut-outs. Its supplied geometry is preserved; Material
recolours it itself. Material's native icon selector remains available.

## Running a room by hand

```
yeney --room="Living Room" [--ip=<sonos-ip>] [--server=<lms-host>]
```

| Flag | Meaning |
| --- | --- |
| `--list-rooms --details` | Sorted tab-separated name, model, IP, coordinator and comma-separated members (coordinator first); `-` means unknown. Uses the room’s primary device for bonded speakers. |
| `--find-server` | Discover LMS; print only its host/IP and exit 0, or print nothing and exit non-zero. |
| `--list-rooms` | Print sorted, unique room names, including group members, then exit. Uses `YENEY_UPNP` and honours `--ip`. |
| `--room=<name>` | Required except with `--list-rooms` or `--find-server`. The Sonos room/zone to take over. |
| `--ip=<address>` | Skip Sonos auto-discovery and talk to this player directly (any player in the household will do -- they share topology). Needed if discovery can't reach the Sonos network. |
| `--server=<host>` | LMS hostname or IP **only**, not a web-UI port. Precedence: explicit `--server` > `LMS_SERVER=` in `/etc/yeney/config` > automatic UDP broadcast discovery on port 3483 (same subnet only; does not cross routers). |

The first instance binds port 1400 for its own use; a second concurrent instance
(a second room) moves to 1401, and so on.

## Frequently asked questions

**Can I play Lyrion Music Server (LMS) on Sonos speakers?**
Yes. That is exactly what YeneY does: each Sonos room becomes an LMS player that
you control from any LMS app.

**Does it work with Logitech Media Server or Squeezebox Server?**
Yes. Logitech Media Server and Squeezebox Server are the old names of Lyrion
Music Server.

**Do I need the Sonos app?**
No. You control playback from LMS. The Sonos app keeps working next to it: pause
and play in the Sonos app are passed back to LMS.

**Does YeneY work with Sonos S1 or S2?**
It is tested on speakers running the S2 software. S1 has not been tested.

**Can I use internet radio and streaming services?**
Yes. Anything LMS can play reaches the Sonos as lossless FLAC, including radio
stations and the streaming-service plugins you use in LMS.

**Is it gapless? Does crossfade work?**
Yes. YeneY sends one continuous stream, so albums play gapless and LMS crossfade
works.

**Can I group Sonos rooms?**
Yes. When rooms are grouped in the Sonos app, bridging a member room controls the
whole group.

**Does it synchronise with my Squeezebox players?**
Not yet. The Sonos starts a few seconds after other LMS players. Precise
synchronisation is planned.

**Can I skip tracks or seek from the Sonos app?**
Not yet; use your LMS app for that. It is under investigation.

**Is YeneY free?**
Yes, for noncommercial use, under the
[PolyForm Noncommercial License](LICENSE). See [License](#license).

## Where it falls short

- Next, Previous and seeking work from LMS, not yet from the Sonos app.
- Queue mode is for standalone rooms; grouped rooms use stream mode.
- Queue mode has no automatic fallback to stream mode yet; a failure stops the
  room and is reported to LMS.
- In stream mode the Sonos app shows the artist and title at the start of a
  stream, but does not update them between tracks of a continuous album. Use
  queue mode for per-track titles.
- Sonos rooms do not yet play in sync with other LMS players.
- Sonos only: other UPnP/DLNA speakers are not supported.

The technical background of each point is in
[How YeneY works](docs/how-it-works.md#where-it-falls-short).

The old `YENEY_DEBUG_EVENTS=1`, `YENEY_DEBUG_STREAM=1`, `YENEY_POLL=legacy`
and `YENEY_STOPPED_MEDIAINFO=1` names still work, with one deprecation warning
each at startup. Canonical debug tokens enable the selected experiments; an
enabled legacy alias also keeps its previous effect. All timing/LampaStream and
user-facing playback settings retain their names and defaults.

## For developers

### Building and testing

```sh
make
make test
```

`make test` runs the bridge fixtures and bundled core suite without a physical speaker.
Test tools also require `flac`, `ffmpeg`, and `clang-format` (set `CLANG_FORMAT`
to its executable path when needed). Physical
tests on real Sonos speakers use `scripts/device-test.sh`; see
[Testing and device-test tooling](docs/how-it-works.md#testing-and-device-test-tooling)
and [DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md).

The opt-in ReplayGain measurement runs with `SCENARIOS="9"` (also with `AUTO=1`),
without app prompts. It needs a ReplayGain-tagged track and the `flac` tool
(`apt-get install -y flac`). S9 measures the gain sent by LMS and the level change
in YeneY's streamed FLAC, temporarily changes the player's ReplayGain preference,
then restores it. `RG_TRACK` defaults to `id:47145` (Tony Bennett ft. Lady Gaga,
“The Lady Is A Tramp”, *Duets II*), specific to the owner's LMS library like the
`TRACK_A`/`TRACK_B` defaults. Find your own ID using the LMS CLI on port 9090:
`titles 0 20 search:The%20Lady%20Is%20A%20Tramp tags:alY`, then set
`RG_TRACK="id:<n>"`. A title instead selects the first of up to 20 matches with
ReplayGain and archives all matches in `s9-matches.txt`; none tagged means
INVALID with the matches listed. See the linked testing guide for `RG_SECS`.

Forgot `--recursive` when cloning? `git submodule update --init --recursive` fixes
it after the fact. Existing checkouts upgrading past the removal of the noson
backend should run this once after pulling:

```sh
git submodule deinit -f noson && rm -rf .git/modules/noson noson
```

The separate UPnP device-discovery PNG icon is generated by `python3 scripts/make-icon.py` using only the
Python standard library. The PNG and embedded header are committed, so building
does not need an image tool or a generation step.

### Player engine

The executable uses only yeney-core. `YENEY_PLAYER` is obsolete: unset or `core`
is silent; any other value produces one removal warning and continues with core.
The startup journal identifies it as `Player engine: yeney-core`.
The installer removes a room's `player.conf` only when it is precisely the
single `[Service]` / `Environment=YENEY_PLAYER=core` override (optional final LF).
Custom content and all other overrides, including `debug.conf`, are preserved.

### Source layout

| Path | What lives there |
| --- | --- |
| `yeney.cpp` | Startup: argument parsing, Sonos/LMS discovery, wiring the rest together. |
| `core_sonos.cpp` / `.h` | Core sink, bounded PCM feeder and audible clock. |
| `pcm_pack.h` | Project-owned signed PCM conversion. |
| `third_party/yeney-core/` | Sole player engine and recursive decoder notices. |
| `sbencoder.cpp` / `.h` | FLAC encoding of the decoded PCM, rate-limited relative to real time. |
| `sbstreamer.cpp` / `.h` | The HTTP server Sonos actually connects to for the audio. |
| `resume_state.h` | The "what does this unpause mean" decision logic, isolated from I/O for testing. |
| `stop_debounce.h` | The 400 ms stop/seek debounce logic, likewise isolated. |
| `sonos-status.cpp` / `.h` | UPnP polling of the Sonos device's own transport/track state. |
| `sonos-position.cpp` / `.h` | UPnP polling of actual playback position, exposed to the output thread. |
| `upnp/` | YeneY's own UPnP layer: discovery, SOAP control, GENA events, HTTP serving. |

### Further reading

- [How YeneY works](docs/how-it-works.md): design, the Sonos pause problem and how
  it was found, metadata findings, the UPnP layer and test tooling.
- [The UPnP layer and wire fixtures](docs/upnp-layer.md).
- [DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md): which physical checks a change needs.

## Related

- [philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP) -- the mature,
  general-purpose LMS-to-UPnP bridge; see the comparison at the top.
- [Lyrion Music Server](https://lyrion.org) -- the music server YeneY plays from.
- [janbar/noson](https://github.com/janbar/noson) -- the Sonos library by
  Jean-Luc Barriere that YeneY started from; no longer included.
## License

Project-owned code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE)
(`PolyForm-Noncommercial-1.0.0`). See the license for permitted purposes and terms.

The executable is PolyForm Noncommercial 1.0.0 apart from the components listed
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which retain their own terms.

Historical note: releases before the core-only removal commit contained
GPL-3.0 squeezelite and remain available under the GPL; history is unchanged.

Sonos is a trademark of Sonos, Inc. YeneY is an independent project and is not
affiliated with or endorsed by Sonos, Inc., Logitech or the Lyrion project.

Copyright (C) 2026 Jaap van Vliet

Grouped HTTP recovery and investigation: [Decisions and findings](docs/grouped-range-recovery.md).

Grouped playback reserve: `YENEY_START_LEAD_MS=1000` (default; valid 0–10000; 0 retains legacy pacing). Set `YENEY_DEBUG=stream` for per-response lead and TCP diagnostics. See [decisions and device trials](docs/start-lead.md).

The opt-in playback-clock probe is documented in
[Timing probe](docs/timing-probe.md). Enable it with `YENEY_TIMING_PROBE=1`; it
never retimes audio. A locked clock now supplies continuous LMS position by
default; `YENEY_LMS_POSITION_FROM_MODEL=0` restores RelTime-only reporting. With
`YENEY_TIMING_PUBLISH=1`,
LampaStream can read the opt-in SHM timing contract; inspect it with
`scripts/yeney-timing-read <mac>`.

Queue audio is 24-bit with a 48 kHz speaker cap. Modern queue-configured
rooms advertise 192 kHz and yeney-core converts native local FLAC/ALAC with
an integer-ratio linear-phase FIR: 88.2/176.4 kHz → 44.1 kHz and 96/192 kHz
→ 48 kHz. Exact source counts give exact WAV lengths immediately, without
waiting for LMS/SoX to finish a conversion. Remote sources use the stream
path and the same conversion when necessary. Rates at or below 48 kHz pass
through unchanged. Ordinary stream-mode rooms retain LMS/SoX conversion
and their 48 kHz advertisement. Legacy `YENEY_AUDIO=16/44` retains the
44.1 kHz advertisement and LMS conversion. Unsupported rate families and speaker errors
stop/release queue playback and report failure to LMS. Queue diagnostics expose
`rendered_rate` for the current owned item. `YENEY_QUEUE_POLL_MS` defaults
to 500 ms; the relay checks every 100 ms during the first three seconds after
Play/Seek/track changes and the final three seconds of an audible item. GENA
events trigger fresh observations; successful position/transport polls are silent.

The queue device suite accepts `--remote <favourite name or episode URL>` to
choose its remote-source scenario. Feed/container entries are browsed for playable
episodes; candidates that produce no SlimProto start within five seconds are
unloaded, up to five attempts. If none is playable the scenario reports SKIP.
See [device verification](DEVICE-VERIFICATION.md#remote-source-selection-in-the-queue-device-suite).
