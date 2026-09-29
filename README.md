# YeneY — play Lyrion Music Server (LMS) on Sonos

**YeneY turns every Sonos speaker into a real Lyrion Music Server player.** Lyrion
Music Server (LMS) was formerly called Logitech Media Server or Squeezebox Server.
Your Sonos rooms appear in LMS next to your Squeezebox players. You control them from any LMS app: Material Skin,
the web interface, iPeng or Squeezer. The music keeps LMS's full sound processing
and reaches the speaker as lossless 24-bit FLAC, gapless, including internet radio
and the streaming services you use through LMS.

No Sonos-specific app, no pairing step per track, and no cloud service: YeneY runs
on your own network, on any small Linux machine, container or VM.

**YeneY** is pronounced **YEN-ee**. The palindrome nods to *Sonos*.

## YeneY compared with LMS-uPnP

[philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP) (UPnPBridge) is the
mature, general-purpose bridge between LMS and UPnP/DLNA speakers, Sonos being one
of many. It has two modes: *per track* (the speaker plays one track at a time) and
*flow* (one continuous stream). YeneY always uses one continuous stream, built
for Sonos only.

| | YeneY | LMS-uPnP, per track | LMS-uPnP, flow mode |
|---|---|---|---|
| LMS sound processing | ✔ ReplayGain, crossfade, DSP | ~ ReplayGain and resampling, no crossfade (none at all in pass-through mode) | ✔ ReplayGain, resampling and crossfade |
| Lossless | ✔ 24-bit FLAC, including radio and streaming services | ✔ FLAC for files; radio passed through as is | ~ lossless, but changing titles need MP3/AAC |
| Gapless | ✔ one continuous stream | ~ via SetNextAVTransportURI; depends on the speaker | ✔ gapless and crossfade |
| Pause/resume from LMS and the Sonos app | ✔ without errors; Sonos-app pause and play passed back to LMS (see [Pause and resume on Sonos](docs/how-it-works.md#pause-and-resume-on-sonos)) | ✔ native per-track pause | ~ Sonos sees a live stream; pause needs a workaround (`live_pause`) |
| Synchronises with other LMS players | ✘ not yet: the Sonos starts playback seconds after other LMS players and reports its position in whole seconds | ? not verified | ? not verified |
| Artist and title in the Sonos app | ~ "artist - title" at the start of each stream; does not yet change between tracks of a continuous album *(work in progress)* | ✔ per track | ~ only with MP3/AAC (ICY) |
| Next/Previous in the Sonos app | ✘ *(under investigation)* | ? not verified | ✘ live stream |
| Seeking in the Sonos app | ✘ only from LMS *(under investigation)* | ? not verified | ✘ live stream |
| Other UPnP/DLNA speakers | ✘ Sonos only | ✔ | ✔ |
| Maturity | new; tested on two Play:1 speakers, a Sonos Port and a grouped room | mature, years of use on many devices | mature |

✔ yes · ✘ no · ~ partly · ? not verified yet

### Which one should I use?

| Choose YeneY if you… | Choose LMS-uPnP if you… |
|---|---|
| control playback from LMS: Material Skin, the web interface, iPeng | mainly control playback from the Sonos app |
| want the full LMS sound: ReplayGain, crossfade, DSP, in lossless 24-bit | want the right track title and Next/Previous in the Sonos app today |
| want pause and resume to work from both LMS and the Sonos app, without error messages | have speakers from other brands as well |
| only have Sonos speakers | prefer a long-proven solution |

**Why YeneY exists.** LMS-uPnP makes you choose between two modes, and each gives
something up. YeneY does everything in one mode: always lossless, gapless, with all
LMS processing. It also solves a Sonos limit that affects any continuous stream: a
Sonos cannot resume a paused radio-style FLAC stream without an error. YeneY works
around that, and passes pause and play in the Sonos app back to LMS. The gap in the
Sonos app itself (titles, Next/Previous, seeking) is being worked on; see
[Why a continuous stream](docs/how-it-works.md#why-a-continuous-stream-and-how-this-differs-from-track-by-track-upnp).

## What you need

- **Sonos speakers on the S2 software.** Tested on a Play:1 (firmware 86.10) and a
  Sonos Port (97.1). S1-only speakers have not been tested.
- **Lyrion Music Server** on your network (formerly Logitech Media Server).
- **A Linux machine for YeneY**, for example a Debian or Ubuntu container or VM.
  It needs to reach the Sonos speakers and LMS on the local network. One small
  process runs per Sonos room.

## How it works in one paragraph

For every Sonos room, YeneY registers a player with LMS, exactly like a Squeezebox
would. LMS sends it the music; YeneY encodes it as one continuous FLAC stream and
tells the Sonos speaker to play that stream, while keeping play, pause, stop,
volume, position and track information in step on both sides. LMS stays in charge
of the playlist and the sound; the Sonos simply plays what it receives. The full
story, including the Sonos pause problem YeneY solves, is in
[How YeneY works](docs/how-it-works.md).

## Installing

The commands use `sudo`; in a container where you are already root, you can leave it out.

Install the build tools and libraries (Debian/Ubuntu):

```sh
sudo apt-get install -y --no-install-recommends \
    git make g++ python3 libssl-dev libflac++-dev \
    libasound-dev libvorbis-dev libfaad-dev libmad0-dev libmpg123-dev libsoxr-dev
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

Kept rooms are **not restarted**. Running rooms restart only for a changed or
missing build record, an LMS setting changed in this run, or `--restart`.
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

Each setting is read and logged once at startup. An invalid value logs a warning
and uses the default.

| Setting | Values | Default |
|---|---|---|
| `YENEY_AUDIO` | `24/48`: 24-bit FLAC at the source's 44.1/48 kHz rate · `16/44`: legacy 16-bit/44.1 kHz | `24/48` |
| `YENEY_TITLE_FORMAT` | `artist-title`: "artist - title" · `title`: title only | `artist-title` |
| `YENEY_STREAM_CONTENT` | `structured`, `plain` or `off`: the radio-text field for other controllers | `structured` |
| `YENEY_PAUSE` | `stop`: the Sonos-safe pause · `pause`: plain UPnP Pause (fallback) | `stop` |
| `YENEY_POLL` | `events`: event-driven · `legacy`: the older polling schedule, for A/B tests | `events` |
| `YENEY_STOPPED_MEDIAINFO` | `0` or `1`: poll media info while stopped | `0` |

The details of each setting are in [How YeneY works](docs/how-it-works.md).

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

- The Sonos app shows the artist and title at the start of a stream, but does not
  yet update them between tracks of a continuous album.
- Next, Previous and seeking work from LMS, not yet from the Sonos app.
- Sonos rooms do not yet play in sync with other LMS players.
- Sonos only: other UPnP/DLNA speakers are not supported.

The technical background of each point is in
[How YeneY works](docs/how-it-works.md#where-it-falls-short).

## Upgrading from sonos-lms

YeneY was previously called sonos-lms. On the machine that runs it, pull in the
old checkout and run the migration as root:

```sh
cd /opt/sonos-lms
git pull --ff-only
sudo scripts/migrate-from-sonos-lms.sh
cd /opt/yeney
journalctl -u 'yeney@*' -f
```

The script finds enabled `sonos-lms@<room>` services in
`/etc/systemd/system/*.wants/` and also includes currently active instances.
It prints each room's source before changing anything. If the old checkout exists
but no rooms are found, it stops with an error. To include rooms explicitly by
display name, run `sudo scripts/migrate-from-sonos-lms.sh --rooms "Study,Sonos Port,MBR"`.
These names are added to any discovered rooms; an interrupted migration resumes
its recorded room list.

The script records and stops the selected services, moves
`/opt/sonos-lms` and `/etc/sonos-lms` to `/opt/yeney` and `/etc/yeney`, and moves
the room drop-ins (including names with spaces). It renames their settings,
removes `SONOS_LMS_EVENT_PORT` lines, updates the Git remote, removes the old
unit template, reloads systemd, builds, and installs the same rooms as
`yeney@<room>`. Playback stops during migration. It stops at the first error;
if interrupted after the checkout moves, rerun
`sudo /opt/yeney/scripts/migrate-from-sonos-lms.sh`. After success, rerunning
finds nothing to do. Conflicting old and new directories are never overwritten.

Only the new `YENEY_` settings are read. Startup warns once per old
`SONOS_LMS_*` or `SONOS_SQUEEZEBOX_*` variable, naming its replacement when one
exists; unknown settings are obsolete. Update any settings outside the migrated
drop-ins yourself. The old stream path is rejected like a stale session so a
speaker holding a pre-upgrade URI cannot attach it to a new stream.

## For developers

### Building and testing

```sh
make
make test
```

`make test` runs the unit tests and fixtures without a physical speaker. Physical
tests on real Sonos speakers use `scripts/device-test.sh`; see
[Testing and device-test tooling](docs/how-it-works.md#testing-and-device-test-tooling)
and [DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md).

Forgot `--recursive` when cloning? `git submodule update --init --recursive` fixes
it after the fact. Existing checkouts upgrading past the removal of the noson
backend should run this once after pulling:

```sh
git submodule deinit -f noson && rm -rf .git/modules/noson noson
```

The original icon is generated by `python3 scripts/make-icon.py` using only the
Python standard library. The PNG and embedded header are committed, so building
does not need an image tool or a generation step.

### Source layout

| Path | What lives there |
| --- | --- |
| `yeney.cpp` | Startup: argument parsing, Sonos/LMS discovery, wiring the rest together. |
| `slimproto_sonos.c` | The unmodified upstream `slimproto.c`, plus an interception point for `strm` transport commands. |
| `output_sonos.c` / `.h` | Squeezelite output backend; silent/non-silent detection, feeds the encoder. |
| `sbencoder.cpp` / `.h` | FLAC encoding of the decoded PCM, rate-limited relative to real time. |
| `sbstreamer.cpp` / `.h` | The HTTP server Sonos actually connects to for the audio. |
| `resume_state.h` | The "what does this unpause mean" decision logic, isolated from I/O for testing. |
| `stop_debounce.h` | The 400 ms stop/seek debounce logic, likewise isolated. |
| `sonos-status.cpp` / `.h` | UPnP polling of the Sonos device's own transport/track state. |
| `sonos-position.cpp` / `.h` | UPnP polling of actual playback position, exposed to the output thread. |
| `upnp/` | YeneY's own UPnP layer: discovery, SOAP control, GENA events, HTTP serving. |
| `squeezelite/` | GPL-3.0 submodule, our fork -- see `.gitmodules`. Commit changes in the fork before updating its pin. |

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
- [ralph-irving/squeezelite](https://github.com/ralph-irving/squeezelite) --
  the LMS player YeneY is built on.

## License

Project-owned code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE)
(`PolyForm-Noncommercial-1.0.0`). See the license for permitted purposes and terms.

noson is no longer included. Squeezelite retains its
[GPL-3.0 license](squeezelite/LICENSE.txt).
This change does not relicense third-party code or revoke rights granted by
previous GPL releases.

The current executable includes GPL-covered squeezelite. PolyForm Noncommercial's
noncommercial restriction is incompatible with distributing that combined work
under the GPL. Distributing a combined executable requires resolving this
conflict, for example through separate permission from the relevant copyright
holders or replacement of the GPL-covered code. See the
[GNU GPL FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#GPLIncompatibleLibs).

Sonos is a trademark of Sonos, Inc. YeneY is an independent project and is not
affiliated with or endorsed by Sonos, Inc., Logitech or the Lyrion project.

Copyright (C) 2026 Jaap van Vliet
