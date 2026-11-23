# YeneY — play Lyrion Music Server (LMS) on Sonos

**YeneY turns every Sonos speaker into a real Lyrion Music Server player.** Lyrion
Music Server (LMS) was formerly called Logitech Media Server or Squeezebox Server.
Your Sonos rooms appear in LMS next to your Squeezebox players, with their own Sonos
icon in Material Skin. You control them from any LMS app (Material Skin, the web
interface, iPeng, Squeezer) **and from the Sonos app**. LMS stays in charge of the
playlist and the sound.

**What sets YeneY apart:**

- **Sound quality.** Lossless 24-bit audio. At 44.1 and 48 kHz the samples LMS
  produces reach the Sonos unchanged (bit-perfect when ReplayGain is off; YeneY
  never applies LMS volume to the audio). Hi-res files (88.2–192 kHz) are converted
  to the Sonos maximum by YeneY's own converter, measured equal to SoX at its
  highest quality setting (`rate -v`): passband ripple below ±0.0002 dB and
  aliasing below −120 dBFS ([measurements](docs/designs/queue-hires-decimation.md)).
- **Your LMS playlist in the Sonos app.** Every LMS track is its own row in the
  Sonos queue, with title, artist, album, artwork and duration: the whole playlist,
  up to 500 rows by default.
- **The Sonos app controls LMS.** Play, pause, stop, Next, Previous, seeking and a
  tap on any row in the Sonos app all reach LMS. The speaker moves by itself and
  YeneY reports its choice to LMS.
- **Timing.** YeneY reads the playback position from the speaker's own clock in
  milliseconds. On a cabled Sonos Port the position LMS shows was within a median
  of 1.3 ms of the speaker (measured 19 November 2026). The same timing is
  available to other programs, for example for music-reactive lighting
  ([LampaStream](#related)).
- **Integration with LMS.** YeneY behaves like a well-mannered LMS player: LMS's
  playlist index stays in step with what the speaker plays, YeneY sets its own
  player icon, and it never sends LMS commands in reaction to LMS's own messages.

No cloud service and no Sonos account: YeneY runs on your own network, on any small
Linux machine, container or VM.

**YeneY** is pronounced **YEN-ee**. The palindrome nods to *Sonos*.

## Status

Version 0.9.0. YeneY is new and under active development. It is used daily on two
Sonos Play:1 speakers and a Sonos Port. See [Known problems](#known-problems)
before you rely on it.

## Two playback modes

| Mode | How it plays | Used for |
|---|---|---|
| **Queue** (default) | One Sonos queue row per LMS track. Exact-length rows are served as 24-bit FLAC by default. WAV is selectable per room. Finite remote and podcast rows with a length margin use WAV. | Local music, podcasts, remote tracks with a known length |
| **Stream** | One continuous 24-bit FLAC stream per room, paced on the Sonos clock | Live radio and sources without a length; chosen automatically in queue mode. Can be forced with `YENEY_PLAYBACK_MODE=stream` |

## YeneY compared with LMS-uPnP and Music Assistant

[philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP) (UPnPBridge) is the
mature, general-purpose bridge between LMS and UPnP/DLNA speakers, Sonos being one
of many. [Music Assistant](https://www.music-assistant.io) is a music server of its
own with a Sonos player provider; it does not play the LMS library. YeneY is built
for Sonos and LMS only.

| | YeneY | LMS-uPnP | Music Assistant (Sonos S2) |
|---|---|---|---|
| Transport | Exact-length FLAC rows by default, WAV selectable per room. Continuous FLAC for live sources. | Continuous HTTP stream | Queued track URLs |
| Plays the LMS library | ✔ | ✔ | ✘ its own library and sources |
| Lossless 24-bit | ✔ 24-bit throughout; bit-perfect at 44.1/48 kHz with ReplayGain off | ~ FLAC by default; 24-bit can be set, not verified on Sonos | ✔ FLAC by default; 24-bit on current models |
| Hi-res sources (88.2–192 kHz) | ✔ YeneY's own converter, measured equal to SoX `rate -v` | ✔ resampled by LMS or the bridge | ✔ limited to 48 kHz |
| Title, artist, album and artwork per track in the Sonos app | ✔ | ✔ | ✔ |
| The whole playlist visible in the Sonos app | ✔ row for row, up to 500 rows by default | ✘ does not use the Sonos queue | ? the speaker holds a small window of tracks; what the app shows is not verified |
| Gapless | ✔ | ✔ (not for tracks of unknown length) | ✔ |
| Play, pause and stop in the Sonos app reach the server | ✔ | ✔ | ✔ (its own pause is sent as a stop) |
| Next, Previous and seeking in the Sonos app | ✔ ([one known fault](#known-problems)) | ✘ not possible in this design | ~ Next and Previous; seeking is switched off |
| Tap any row in the Sonos app | ✔ | ✘ | ? not verified |
| A seek from the server keeps the track's row | ✔ | not applicable | ✘ the queue is loaded again |
| Playback position reported to the server | ✔ milliseconds, from the speaker's own clock | ~ polled from the speaker, whole seconds | ~ milliseconds as they arrive, used for the progress bar |
| Volume both ways | ✔ optional, with a maximum | ✔ | ✔ |
| Sonos groups | ~ plays through the group's leading room; a member room stops cleanly | ✔ follows groups | ✔ makes and follows groups |
| Synchronises with other LMS players | ✘ not yet | ~ start only | not applicable |
| Home Assistant | ✔ through the LMS integration; shown as linked to the Sonos device | ✔ through the LMS integration | ✔ its own integration |
| Song titles on internet radio | ~ at the start of a stream | ✔ | ? not verified |
| Timing export for other programs (lighting) | ✔ | ✘ | ✘ |
| Automated tests in the repository | ✔ | ✘ | ✔ |
| Packaging | ✘ build from source on Linux | ✔ LMS plugin for all platforms | ✔ Home Assistant add-on and container |
| Other speaker brands | ✘ Sonos only | ✔ any UPnP/DLNA renderer | ✔ many brands |
| Maturity | new | mature, years of use on many devices | stable, widely used |

✔ yes · ✘ no · ~ partly · ? not verified

How this table was made: LMS-uPnP was read at version 3.4.10 (October 2026) and
Music Assistant at revision `2506795` of its server (19 November 2026), from their
source and documentation. Music Assistant was not run on our speakers. Corrections
are welcome.

### Which one should I use?

| Choose YeneY if you… | Choose LMS-uPnP if you… | Choose Music Assistant if you… |
|---|---|---|
| use LMS and only have Sonos speakers | use LMS and have speakers of other brands as well | do not need LMS and want one server for many brands and services |
| want your LMS playlist in the Sonos app, and the Sonos app to control LMS | prefer a long-proven solution | live in Home Assistant |
| want the best sound on Sonos: 24-bit, bit-perfect at 44.1/48 kHz | want an LMS plugin that installs on any platform | group Sonos and other speakers a lot |
| want millisecond timing, for example for music-reactive lighting | | |

## What you need

- **Sonos speakers on the S2 software.** Tested on the Play:1 (firmware 86.10) and
  the Sonos Port (97.x). S1-only speakers have not been tested.
- **Lyrion Music Server** on your network (formerly Logitech Media Server).
- **A Linux machine for YeneY**, for example a Debian or Ubuntu container or VM.
  It needs to reach the Sonos speakers and LMS on the local network. One small
  process runs per Sonos room.

A speaker on a network cable gives the steadiest timing. A speaker that reaches the
network through another Sonos product's radio works too, with slower position
answers for about 20 s after a start, skip or seek (see
[Known problems](#known-problems)).

## How it works in one paragraph

For every Sonos room, YeneY registers a player with LMS, exactly like a Squeezebox
would. LMS sends it the music. YeneY counts the audio exactly and offers each LMS
track to the speaker as one row in the Sonos queue, with its exact length. The
speaker fetches the audio from YeneY and plays it; it never plays your files
itself. When you press Next or seek, in LMS or in the Sonos app, the speaker moves
and YeneY keeps LMS in step, using the position from the speaker's own clock. Live
radio and sources without a length are sent as one continuous FLAC stream instead.
More background is in [How YeneY works](docs/how-it-works.md).

## Home Assistant

YeneY registers one LMS player per bridged Sonos speaker under the speaker's
own network address. The LMS model text shows the Sonos product, for example
`Sonos Play:1 (YeneY)`. From Home Assistant 2026.9, the YeneY and Sonos devices
appear as linked devices because they share that address. You can browse the
LMS library in Home Assistant and play it on the Sonos speaker.

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
and restarts only the rooms that run an older build. All its questions, flags and
the configuration file are described in [Installer details](docs/installer.md).

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
The most used settings are below; an invalid value logs a warning and uses the
default (an invalid queue disk quota stops queue mode). Player name, volume,
artwork, the Material Skin icon and the older debug names are explained in
[Settings in detail](docs/settings-details.md).

| Setting | Values | Default |
|---|---|---|
| `YENEY_PLAYBACK_MODE` | `queue`: source-aware relay/stream on the coordinator · `stream`: explicit continuous transport · `auto`: stream, logged as `auto-phase4` | `queue` |
| `YENEY_QUEUE_FORMAT` | `flac` or `wav` for exact-length queue rows. Use `yeney-format` to switch a room. | `flac` |
| `YENEY_QUEUE_PACE_TAIL_MS` | Milliseconds at the end of each queue row exempt from the delivery rate limit | `0` |
| `YENEY_QUEUE_PACING` | Limit the delivery rate of queue audio. Set to `off` to disable. | `on` |
| `YENEY_QUEUE_POLL_MS` | Queue steady speaker poll interval, 100–2000 ms; starts and joins use 100 ms windows | `500` |
| `YENEY_QUEUE_NEXT_DELAY_S` | 1–120 seconds: request the remote successor only below this remaining audible time, after full production | `20` |
| `YENEY_QUEUE_DISK_MIB` | Positive disk-history quota in MiB per queue room; explicit values apply to both paths | `256` local / `8192` estimated |
| `YENEY_SHOW_SONOS_SUFFIX` | `yes`: LMS displays `<room> (Sonos)` · `no`: LMS displays `<room>`; identity stays unchanged | `yes` |
| `YENEY_LMS_VOLUME` | `off`: ignore LMS volume · `speaker`: map LMS volume/mute to Sonos and mirror app changes to LMS; PCM remains unchanged | `off` |
| `YENEY_VOLUME_MAX` | Maximum speaker volume YeneY writes, integer 0–100; applies in speaker mode | `100` |
| `YENEY_LMS_ICON` | Material icon name; explicitly empty disables preference handling | `sonos` |
| `YENEY_START_LEAD_MS` | Startup reserve, 0–10000 milliseconds | `1000` |
| `YENEY_UPNP` | `yeney` or `own`; unsupported names warn and use YeneY | `yeney` |
| `YENEY_STREAM_PACING` | `sonos`: follow the Sonos clock · `wall`: legacy host clock opt-out | `sonos` |
| `YENEY_AUDIO` | `24/48`: 24-bit FLAC at the source's 44.1/48 kHz rate · `16/44`: legacy 16-bit/44.1 kHz | `24/48` |
| `YENEY_PODCAST_TITLE_PLAIN` | `yes`: plain native Podcast episode title; `no`: LMS formatted title unchanged | `yes` |
| `YENEY_PODCAST_ARTIST_SHOW` | `yes`: show podcast name as inferred artist; `no`: omit it | `yes` |
| `YENEY_PODCAST_ALBUM_PUBLISHER` | `yes`: show publisher as inferred album; `no`: omit it | `no` |
| `YENEY_TITLE_FORMAT` | Stream path only: `artist-title`: "artist - title" · `title`: title only; queue items use the plain title (native Podcast settings below can retain its formatted title) | `artist-title` |
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
| `YENEY_QUEUE_ROWS` | 1–60000: the largest number of LMS playlist rows shown in the Sonos queue | `500` |
| `YENEY_POSITION_SOURCE` | `auto`: millisecond position from the speaker, whole seconds as fallback · `websocket` · `upnp`: whole seconds only | `auto` |

### LMS player icon (Material Skin)

YeneY brings its own Sonos icon (`assets/sonos.svg`) and selects it for each room.
When LMS runs on the same machine, tell the installer once where the LMS
preferences are:

```sh
sudo make install LMS_PREFS_DIR=/absolute/path/to/lms/preferences
```

When LMS runs on another machine, or to choose another icon, see
[Settings in detail](docs/settings-details.md#lms-player-icon-material-skin).

## Running a room by hand

```
yeney --room="Living Room" [--ip=<sonos-ip>] [--server=<lms-host>]
```

| Flag | Meaning |
| --- | --- |
| `--list-rooms --details` | Sorted tab-separated name, model, IP, coordinator and comma-separated members (coordinator first); `-` means unknown. Uses the room’s primary device for bonded speakers. |
| `--find-server` | Discover LMS; print only its host/IP and exit 0, or print nothing and exit non-zero. |
| `--list-rooms` | Print sorted, unique room names, including group members, then exit. Uses `YENEY_UPNP` and honours `--ip`. |
| `--lms-volume=off\|speaker` | Override the room's volume mode; takes precedence over its environment and saved config. |
| `--show-sonos-suffix=yes\|no` | Override the LMS display suffix; true name and MAC remain unchanged. |
| `--room=<name>` | Required except with `--list-rooms` or `--find-server`. The Sonos room/zone to take over. |
| `--ip=<address>` | Skip Sonos auto-discovery and talk to this player directly (any player in the household will do -- they share topology). Needed if discovery can't reach the Sonos network. |
| `--server=<host>` | LMS hostname or IP **only**, not a web-UI port. Precedence: explicit `--server` > `LMS_SERVER=` in `/etc/yeney/config` > automatic UDP broadcast discovery on port 3483 (same subnet only; does not cross routers). |

The first instance binds port 1400 for its own use; a second concurrent instance
(a second room) moves to 1401, and so on.

## Frequently asked questions

**Can I play Lyrion Music Server (LMS) on Sonos speakers?**
Yes. That is exactly what YeneY does: each Sonos room becomes an LMS player.

**Does it work with Logitech Media Server or Squeezebox Server?**
Yes. Those are the old names of Lyrion Music Server.

**Do I need the Sonos app?**
No. You can control everything from LMS. The Sonos app works next to it: play,
pause, stop, Next, Previous, seeking and a tap on a row are passed back to LMS, and
the app shows your LMS playlist.

**Does YeneY work with Sonos S1 or S2?**
It is tested on speakers running the S2 software. S1 has not been tested.

**Can I use internet radio, podcasts and streaming services?**
Yes. Anything LMS can play reaches the Sonos losslessly. Podcasts and remote tracks
with a known length become queue rows; live radio is sent as one continuous stream.

**Is it gapless? Does crossfade work?**
Gapless: yes, in both modes. LMS crossfade works in stream mode; in queue mode it
is not available yet.

**Can I group Sonos rooms?**
Partly. Play through the group's leading room (the coordinator). A member room
refuses playback with one LMS stop and never changes the group or controls another
room. Adding a room to a group in the Sonos app stops that room's own LMS player.

**Does it synchronise with my Squeezebox players?**
Not yet. Precise synchronisation is planned and builds on the millisecond position.

**Is YeneY free?**
Yes, for noncommercial use, under the
[PolyForm Noncommercial License](LICENSE). See [License](#license).

## Known problems

As of 19 November 2026:

- **End of a playlist.** After the last track the room can stay on "playing" in
  LMS, and a new start inside the same playlist can then fail. Starting another
  playlist works.
- **Next in the Sonos app** can restart the new track from the beginning after
  about a second, when that track had been prepared as the gapless follow-on.
- **Repeat and shuffle set on the speaker itself** are not yet overruled by LMS;
  switch them off in the Sonos app.
- **A seek far ahead early in a track** can show 0:00 in the Sonos app for that
  track.
- **A speaker on Sonos's own radio link** answers position questions slowly for
  about 20 s after a start, skip or seek; LMS can be 0.1–0.2 s behind in that time.
- **Sonos rooms do not yet play in sync** with other LMS players.
- **Crossfade** is not available in queue mode.
- **Internet radio** shows the title at the start of a stream and does not follow
  song changes.
- **No automatic fallback** from queue mode to stream mode; a failure stops the
  room and is reported to LMS.
- **Sonos only**: other UPnP/DLNA speakers are not supported.

## For developers

Build and test:

```sh
make
make test
```

`make test` runs the complete suite without a physical speaker; `make test-fast`
is the quick check. Units are metric (SI) only. Everything else is in
[Developer notes](docs/developer-notes.md): the Sonos local interface and its
sources, the test runner, the source layout and technical notes by subject. See
also [How YeneY works](docs/how-it-works.md),
[the UPnP layer](docs/upnp-layer.md) and
[DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md).

## Related

- [philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP): the mature,
  general-purpose LMS-to-UPnP bridge.
- [Music Assistant](https://www.music-assistant.io): a music server with its own
  Sonos support.
- [Lyrion Music Server](https://lyrion.org): the music server YeneY plays from.
- [LampaStream](https://github.com/ThaYapeMan/LampaStream): music-reactive Philips
  Hue lighting that uses YeneY's timing.
- [janbar/noson](https://github.com/janbar/noson): the Sonos library by
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
