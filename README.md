# YeneY

Make a Sonos speaker behave like a real [Lyrion Music Server](https://lyrion.org)
(LMS, formerly Logitech Media Server) player: controllable from any LMS app
(Material, iPeng, Squeezer, ...), with LMS's full sound processing,
in lossless 24-bit FLAC, and driven entirely through the standard slimproto
protocol -- no Sonos-specific app, no separate remote, no manual pairing step per
track.

**YeneY** is pronounced **YEN-ee**. The palindrome nods to *Sonos* and to
[noson](https://github.com/janbar/noson), the library this project started on:
YeneY is the "yes" answer to noson.

> **Name change in progress.** This project was called *sonos-lms*. The repository,
> the binary, the systemd services (`sonos-lms@<room>`), the settings
> (`SONOS_LMS_*`) and the paths (`/opt/sonos-lms`, `/etc/sonos-lms`) still use the
> old name until the rename is completed. The commands below are the ones that work
> today.

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
| Pause/resume from LMS and the Sonos app | ✔ without errors; Sonos-app pause passed back to LMS (see [Pause and resume on Sonos](#pause-and-resume-on-sonos)) | ✔ native per-track pause | ~ Sonos sees a live stream; pause needs a workaround (`live_pause`) |
| Synchronises with other LMS players | ✔ | ? not verified | ? not verified |
| Track title in the Sonos app | ✘ does not change during an album *(work in progress)* | ✔ per track | ~ only with MP3/AAC (ICY) |
| Next/Previous in the Sonos app | ✘ *(under investigation)* | ? not verified | ✘ live stream |
| Seeking in the Sonos app | ✘ only from LMS *(under investigation)* | ? not verified | ✘ live stream |
| Other UPnP/DLNA speakers | ✘ Sonos only | ✔ | ✔ |
| Maturity | tested on Study, a Sonos Play:1 | mature, years of use on many devices | mature |

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
[Why a continuous stream](#why-a-continuous-stream-and-how-this-differs-from-track-by-track-upnp).

## The problem this solves

LMS knows how to talk to Squeezebox hardware and to squeezelite instances. It has
no idea what a Sonos speaker is. Sonos, in turn, expects to be driven through its
own UPnP/SOAP control surface and to pull audio from an HTTP URL it is handed --
not to receive a slimproto stream.

This project sits between the two. It is squeezelite itself (same decode, buffer
and stream pipeline LMS already trusts), but with the ALSA output swapped for a
custom backend that talks to a Sonos device instead of a sound card. From LMS's
point of view, a Sonos room is just another squeezelite client. From Sonos's point
of view, it is being handed a normal HTTP audio URL to play, the same as if you had
pasted a stream link into the Sonos app.

## How the pieces fit together

Each running instance represents exactly one Sonos room. On startup it discovers
the Sonos device (or connects to a given IP), discovers the LMS server (or uses
`--server`), derives a stable player identity from the Sonos player's UUID, and
launches squeezelite against that identity as if it were any other client.

From there, three loosely-coupled pieces keep the two sides in sync:

**Getting audio out.** The output backend does not push PCM to a sound card; it
watches squeezelite's own silent/non-silent flag. A transition into audio starts a
FLAC encoder and opens an HTTP endpoint (`/music/squeezebox.flac`) that Sonos is
told to `PlayStream()`. A transition back to silence tears the stream down and
stops the speaker. The encoder deliberately stays only a couple of seconds ahead
of real time -- Sonos buffers aggressively on its own, and letting the encoder run
far ahead only made that worse and confused LMS's own progress tracking.

**Keeping transport commands sane.** LMS's `p`/`q`/`s`/`u` commands and the Sonos
device's own pause/play events arrive independently and can race. Two small,
independently testable state machines absorb that: one decides what an LMS
"unpause" actually means right now (a genuinely new stream, resuming a held HTTP
request, or reissuing the play command against the same URL), and the other
delays turning a stop into an actual device pause by 400 ms, because a drag-seek
in the UI arrives as a stop immediately followed by a new play -- without the
delay, every seek would cause an audible blip on the speaker.

**Keeping LMS's numbers honest.** squeezelite counts frames as it decodes them,
which runs well over a second ahead of what the Sonos speaker is physically
outputting once its own network and playback buffering is accounted for. A
background poll of the Sonos device's actual transport position (via UPnP) feeds
a corrected figure back into the same counter LMS reads for its progress bar and
`ms_played` calculation, so the displayed position tracks what you actually hear
rather than what has merely been decoded. A zero speaker position remains zero
while Sonos buffers, including its early startup reconnect. STAT reports use the
speaker-derived frame difference without a wall-clock fallback or extrapolation
from a blocked output pump, and never move backwards within a track.

## Why a continuous stream, and how this differs from track-by-track UPnP

There are two basic ways to make LMS music come out of a Sonos speaker. Both use
UPnP to control the speaker -- the difference is **who produces the audio and who
is in charge of the playlist**.

**Track-by-track (the usual UPnP renderer approach).** The Sonos is handed one
track at a time -- either as a file URL or as a queue of tracks -- and plays each
one itself. This is how a Sonos plays its own music library, and how most
UPnP/DLNA bridges work by default.

**Continuous stream (this project).** The Sonos is handed a single, never-ending
FLAC stream, the way it would play an internet radio station. The audio is
produced by squeezelite under full LMS control: LMS decides what plays, when, and
how it sounds; the Sonos simply renders what it receives.

The comparison table at the top shows the trade-off for YeneY and both LMS-uPnP
modes.

**In short:** a track-by-track approach mainly improves things *in the Sonos
app*. If you control playback from LMS -- Material Skin, the web interface,
iPeng -- what you gain is limited, and what you give up (LMS sound processing and
synchronisation) is real. YeneY deliberately chooses the continuous stream and
treats the Sonos as a real LMS player.

The three ✘ points share one root cause: Sonos treats the stream as internet
radio, and radio has no next track, no seek bar, and only updates its title via
ICY metadata, which Sonos requests for MP3/AAC but never for FLAC (see
[Where it falls short](#where-it-falls-short)). Status:

- **Track titles -- work in progress.** Solvable within stream mode, as options:
  start a new stream at each track change (correct titles, but a short gap between
  tracks -- not for gapless albums or DJ mixes), or a lossy MP3/AAC stream that
  carries ICY titles.
- **Next/Previous and seeking in the Sonos app -- under investigation.** Not
  possible while the Sonos sees a radio stream. The idea being explored: give the
  Sonos one item per track that still points to this bridge, so LMS keeps producing
  the audio (with all its processing) while the Sonos app gains Next, Previous and a
  seek bar. Whether Sonos accepts this without new pause or gapless problems still
  has to be proven on real speakers.

## Audio quality

| Setting | Values | Default |
|---|---|---|
| `SONOS_LMS_AUDIO` | `24/48`: 24-bit FLAC at the source's 44.1/48 kHz rate; `16/44`: legacy 16-bit/44.1 kHz | `24/48` |

The setting is read once at startup and logged. Invalid values warn and use
`24/48`. Restart the bridge after changing it, for example with a systemd
`Environment=SONOS_LMS_AUDIO=16/44` override for comparison or older speakers.
The default targets Sonos S2's 24-bit/48 kHz FLAC support.

At 44.1 and 48 kHz, decoded PCM keeps its sample rate and up to 24 bits of
precision. A 16-bit source is padded with zero bits, without changing its sample
values. Bit-exact playback assumes LMS ReplayGain, DSP, crossfade and other sample
processing are disabled; lossy sources remain lossy.

The output driver lists 48,000 and 44,100 Hz. Slimproto advertises
`MaxSampleRate=48000` (a maximum, not an exact rate whitelist), so LMS performs
any necessary downsampling before sending audio. With LMS's standard transcoding
configuration and working resampler, 88.2 kHz becomes 44.1 kHz, and 96/192 kHz
becomes 48 kHz. LMS sync-group limits or custom transcoding settings can lower
that further. See [LMS's sample-rate selection](https://github.com/LMS-Community/slimserver/blob/public/9.0/Slim/Player/CapabilitiesHelper.pm).
Legacy mode advertises `MaxSampleRate=44100` and retains the previous 16-bit path
and per-track stream restarts.

A FLAC header fixes its rate for that stream. Natural playlist continuation at
the same rate keeps the stream gapless; a rate change creates a new stream ID and
uses the normal PlayStream path, so a short gap is possible. Explicit seeks and
track replacements still start a new stream. Each new stream logs, for example,
`stream 12: FLAC 24-bit 48000 Hz`. DIDL remains `audio/flac`.

## Source layout

| Path | What lives there |
| --- | --- |
| `sonos-lms.cpp` | Startup: argument parsing, Sonos/LMS discovery, wiring the rest together. |
| `slimproto_sonos.c` | The unmodified upstream `slimproto.c`, plus an interception point for `strm` transport commands. |
| `output_sonos.c` / `.h` | Squeezelite output backend; silent/non-silent detection, feeds the encoder. |
| `sbencoder.cpp` / `.h` | FLAC encoding of the decoded PCM, rate-limited relative to real time. |
| `sbstreamer.cpp` / `.h` | The HTTP server Sonos actually connects to for the audio. |
| `resume_state.h` | The "what does this unpause mean" decision logic, isolated from I/O for testing. |
| `stop_debounce.h` | The 400 ms stop/seek debounce logic, likewise isolated. |
| `sonos-status.cpp` / `.h` | UPnP polling of the Sonos device's own transport/track state. |
| `sonos-position.cpp` / `.h` | UPnP polling of actual playback position, exposed to the output thread. |
| `noson/`, `squeezelite/` | Vendored GPL-3.0 submodules, each our own fork -- see `.gitmodules`. Do not hand-edit; changes there are lost on `git submodule update`. |

## Building it

```sh
sudo apt-get install -y --no-install-recommends \
    make cmake g++ python3 libz-dev libssl-dev libflac++-dev libpulse-dev \
    libasound-dev libvorbis-dev libfaad-dev libmad0-dev libmpg123-dev libsoxr-dev

git clone --recursive https://github.com/ThaYapeMan/sonos-lms.git
cd sonos-lms
make
```

Forgot `--recursive`? `git submodule update --init --recursive` fixes it after the
fact.

## Running it

```
sonos-lms --room="Living Room" [--ip=<sonos-ip>] [--server=<lms-host>]
```

| Flag | Meaning |
| --- | --- |
| `--list-rooms --details` | Sorted tab-separated name, model, IP, coordinator and comma-separated members (coordinator first); `-` means unknown. Uses the room’s primary device for bonded speakers. |
| `--find-server` | Discover LMS; print only its host/IP and exit 0, or print nothing and exit non-zero. |
| `--list-rooms` | Print sorted, unique room names, including group members, then exit. Uses `SONOS_LMS_UPNP` and honours `--ip`. |
| `--room=<name>` | Required except with `--list-rooms` or `--find-server`. The Sonos room/zone to take over. |
| `--ip=<address>` | Skip Sonos auto-discovery and talk to this player directly (any player in the household will do -- they share topology). Needed if discovery can't reach the Sonos network. |
| `--server=<host>` | LMS hostname or IP **only**, not a web-UI port. Precedence: explicit `--server` > `LMS_SERVER=` in `/etc/sonos-lms/config` > automatic UDP broadcast discovery on port 3483 (same subnet only; does not cross routers). |
| `--debug` | Raise noson's own logging verbosity. |
| `--file=<path>` | Play one local audio file straight to the Sonos speaker, bypassing LMS entirely -- a quick way to check the Sonos connection and encoder in isolation. |

The first instance binds port 1400 for its own use; a second concurrent instance
(a second room) moves to 1401, and so on.

A run looks roughly like this once both sides are found:

```
$ sonos-lms --room="Living Room"

sonos-lms -- Sonos as an LMS player
Copyright (C) 2026 Jaap van Vliet

Discovering Sonos devices ... found 3
  Kitchen     RINCON_B8E9375C412001400  192.168.1.40:1400
  Living Room RINCON_347E5C1A902001400  192.168.1.41:1400
  Bedroom     RINCON_78F9C0A3512001400  192.168.1.42:1400

Zones:
  Kitchen      -> coordinator: Kitchen
  Living Room  -> coordinator: Living Room
  Bedroom      -> coordinator: Bedroom

Taking over "Living Room" (MAC 34:7E:5C:1A:90:20) ... connected to LMS
```

### Installing room services

From `/opt/sonos-lms`, run `make` then `sudo make install` (root and Python 3
required). The installer shows its build, the LMS host and its source, then a room
table with model, IP, group and bridge state. It warns if LMS discovery differs
from the saved host. An empty `LMS_SERVER` uses discovery; a missing line is added
with the discovered host or an empty value.

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
sonos-lms installer — build abc1234
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
Logs: journalctl -u 'sonos-lms@*' -f
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
`/etc/sonos-lms/installed-build` records the commit (with `-dirty` when applicable)
and binary SHA-256; it is written atomically only after service actions succeed.
The plan lists Start, Restart (with its reason), Stop and Keep, plus any service
enabling, template installation, migration or build-record changes. With no work,
there is no Apply prompt. Declining Apply or pressing Ctrl-C at a prompt changes
no files or services.

Afterward, enabled rooms (including kept rooms) share an LMS CLI check on port
9090, waiting at most 15 seconds for `<room> (Sonos)` with `connected:1`.
A missing host or unreachable CLI skips the check without failing installation;
a running service alone does not prove it reached LMS.

Alternatively, edit `/etc/sonos-lms/config` and run
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

## Testing

```sh
make test
```

Runs three self-contained binaries against simulated sockets -- no physical Sonos
device involved:

- `encoder-test` exercises the FLAC encoding path in `sbencoder.cpp`.
- `resume-state-test` exercises `resume_state.h`/`stop_debounce.h` in isolation.
- `streamer-test` exercises the HTTP broker in `sbstreamer.cpp`: headers, held-GET
  resume, reconnect behaviour, idle timeouts, debounce timing.

Two Python-driven C++ fixtures also extract the production transport functions
and LMS discovery/config parsers. Discovery tests perform no UDP I/O.

These check the transport and stream state machines, but do not prove behavior
on a real speaker. Use the levels below and [DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md)
to choose the physical checks appropriate to a change.

### Device test levels

- **Level 0 — no physical test:** display, logging and installer changes. Run `make` and `make test` locally.
- **Level 1 — unattended:** for stream and pause logic changes, run `sudo env AUTO=1 scripts/device-test.sh` on the deployment host. It discovers the room's coordinator, sends AVTransport Pause/Play directly, allows up to ten seconds after resume for both playback clocks to start, then checks speaker and LMS time advance by at least three seconds within the next six seconds, checks continued position and track changes, and monitors transport status and journal errors. It prints a PASS/FAIL table and exits nonzero if any scenario fails. This measures playback progress; it cannot hear audio or inspect app dialogs.
- **Level 2 — real Sonos app:** occasionally run `sudo scripts/device-test.sh` and follow the existing app prompts, especially to confirm audible playback and app behavior. `QUICK=1` keeps this manual flow with shorter defaults.

AUTO and QUICK default to `SCENARIOS="1 2 5 6 7" S2_ROUNDS=1 LONG_PAUSE=30`;
explicit environment values override these defaults. Normal manual mode retains
scenarios 1–7, three S2 rounds and a 120-second long pause. AUTO requires Python 3,
`./sonos-lms --list-rooms --details`, coordinator reachability on port 1400, LMS CLI
access and the room's journal; run it from the repository directory. The bridge
logs `speaker URI: stream=N session=<token>` independently of the displayed title.
Unknown or external URIs do not count as successful stream detection in AUTO.
For idle diagnostics, run `AUTO=1 SCENARIOS="8" IDLE_SECS=120`: S8 plays track A,
stops LMS and records stream GETs without sending Play. It passes only if Sonos
and LMS remain stopped. S8 is not included by default. Repeated scenarios such as
`SCENARIOS="7 7 7"` have separate results and monitor logs (`S7#1`, `S7#2`, `S7#3`).
The script records the running bridge's UPnP layer from its current service
invocation. Packet captures include all traffic to/from the discovered speaker
coordinator (`SONOS_IP` overrides it), including stream and GENA ports, with a
512-byte snaplen; Slimproto traffic is also retained.
Agents never SSH to or deploy on LXC 113; the owner runs physical tests there.

## Where it falls short

**Artist and album require YeneY.** With `SONOS_LMS_UPNP=yeney`, the LMS artist
and album are sent as `dc:creator` and `upnp:album` in the Sonos DIDL metadata.
Empty values are omitted. The noson backend still sends title and artwork only;
its `PlayStream()` API does not accept artist or album.

The Sonos app ignores `dc:creator` and `upnp:album` for radio items using
`x-rincon-mp3radio`; its now-playing text comes from `r:streamContent` instead.
For YeneY, `SONOS_LMS_STREAM_CONTENT` selects the text sent in that element:

- `structured` (default): `TYPE=SNG|TITLE <title>|ARTIST <artist>|ALBUM <album>`.
  Empty fields are omitted and `|` inside a value becomes `/`.
- `plain`: `<artist> - <title>`, or just `<title>` when artist is empty.
- `off`: empty, preserving the earlier radio-text behavior.

The setting is read and logged once at startup. Invalid values warn and use
`structured`. All text is XML-escaped; `dc:creator` and `upnp:album` are retained.
The noson backend is unaffected.

**In-stream metadata updates don't work.** Updating the "now playing" title
mid-stream (without restarting it) was attempted via Shoutcast-style ICY metadata
injection into the FLAC stream -- the mechanism itself was fully implemented and
tested. It doesn't work because Sonos never sends the `Icy-MetaData: 1` opt-in
header for `audio/flac` requests, and injecting the blocks anyway just corrupts
the stream (`ERROR_CORRUPT_FILE`). Track title and artwork are therefore only ever
set once, at stream start.

Stream URLs include a random token generated once per process start:
`/music/squeezebox.flac?session=<token>&stream=<N>`. The startup log prints
`Stream session: <token>`. GET and HEAD requests with a missing or different
token receive an empty 404 with `Connection: close`, before encoder ownership
or device-resume handling. Old URLs cannot match a reused stream ID after restart.

## Pause and resume on Sonos

Pausing ends the HTTP response and sends UPnP **Stop** by default. Sonos resumes
FLAC radio (`x-rincon-mp3radio` with `audio/flac`) incorrectly from PAUSED:
the first Play can close the GET with ERROR_CORRUPT_FILE and an app dialog.
A plain native-FLAC relay reproduced the pause/resume failure independently of
the bridge. Stopping after pause made all three reference resumes play cleanly;
the bridge's Stop mode was physically verified on 2026-09-25 with status OK,
continued audio, and no dialog.

The app still shows Play. Its fresh GET receives a normal FLAC header and
chunked audio when LMS resumes; an LMS resume without a GET reissues the same
URL. Each connection's RelTime is anchored to its first PCM's track offset so
LMS position continues across reconnects. Before any positive speaker position,
a startup reconnect discards that queued PCM offset from the audible clock. LMS stop (`strm q`) ends HTTP immediately
and sends Stop after 400 ms unless superseded by `strm s`; track changes send no
transport command. STOPPED itself never resumes LMS. Sonos-app Play sends one
LMS `play` per resume attempt and feeds the fresh GET; if LMS starts a new stream,
the waiting GET survives LMS's q/s flush and redirects when the new ID exists.
After a successfully completed q-Stop, an ACTIVE GET followed by a STANDBY GET
for the current stream within 25 ms also counts as device Play, in both backends.
Study captures on September 27 showed Play pairs 0.8–6.8 ms apart and no pairs
during 32 minutes idle. The bridge logs the request IDs and separation, sends
one LMS play, and holds the ACTIVE GET through the normal feed/redirect path.
A single GET still waits for a transport event and times out after five seconds.
Pairs during playback or after pause-Stop do not use this rule. If no
TRANSITIONING/PLAYING is observed within 15 seconds, a single “GET pair not
confirmed” diagnostic is logged without rollback.
Only a GET classified as a device resume after Stop gets this protection;
ordinary q still ends the response. A held resume sends no bytes if the client
closes it, or returns the existing 503 after its five-second deadline. Each
outcome is logged as `held resume GET #N` followed by `-> 302 stream M`, `fed`,
`closed by client`, or `expired`. Explicit `pause=pause` retains deferred Pause.
The physical script defaults to scenarios 1–7; run S7 alone with
`SCENARIOS=7 scripts/device-test.sh`. The S7 fix still needs a physical recheck.
`LMS=<ip>` overrides host detection. Otherwise the script checks config, recent
unit journal, the unit's `ExecStart --server` (both argument forms), then the
full unit journal, and records the source in its step log.
LMS pause/play intent is retained while a transport call or stream restart is
busy; the latest state is applied once ready, with a deferred-transport log.
PlayStream failures retry up to three attempts, one second apart, then wait for
a new stream or transport command. A stream is complete only after success.
A device-resume request expires after five seconds without LMS `strm u`, allowing
another attempt; CLI errors retain that lease instead of retrying every poll.
No environment overrides are needed. `SONOS_LMS_PAUSE=pause` remains an
explicit fallback to UPnP Pause and the previous same-URL resume behavior
(including HTTP 503 for a speculative held GET). The pause switch is read and
logged once at startup; invalid values warn and use `stop`.

### How the fix was found

This was a hard one. Finding it took more than nine hours of structured
troubleshooting over two days (24–25 September 2026), on top of earlier
attempts that went nowhere.

What made it so difficult:

- **The symptom pointed the wrong way.** The dialog blames our stream
  ("could not be played"), and Sonos closes our connection right before it
  appears, so every sign said the bridge was sending something wrong.
- **Sonos documents none of this.** Why a speaker closes a connection, sends a
  HEAD request or raises an error is visible only on the wire and in its UPnP
  events, never in a log.
- **Every plausible fix failed in its own way.** Answering Sonos's resume
  request with 503, closing it, leaving it open, sending only a Play, starting
  the audio at a clean FLAC frame, restarting the stream automatically,
  dropping chunked encoding: each one changed the details and none of them
  removed the dialog. A few brought the music back, but the dialog still appeared.
- **The cause was not in this code at all.** Sonos cannot resume FLAC radio from
  PAUSED, whoever serves it.

Proving that meant building dedicated tooling first:
`scripts/device-test.sh` runs fixed scenarios against a real speaker and
captures the bridge journal, the LMS event stream and status, and the network
traffic of each run. `scripts/reference-test.sh` with `scripts/reference-relay.py`
puts known-good sources (a live MP3 station, a headerless FLAC station, a clean
FLAC file) in front of the same speaker with the same UPnP command. It took at
least a dozen captured runs, and hours of reading packet captures and
cross-checking journals, LMS logs and Sonos event notifications, before a plain
FLAC file served by a minimal relay reproduced the exact same error. Only then
was it clear that the bridge had never been the problem. That pointed to a
simple fix: stop instead of pause.

## The UPnP layer

### The YeneY UPnP layer

YeneY's own UPnP layer is a small discovery, SOAP control and event layer. It
gradually replaces [noson](https://github.com/janbar/noson), the library by
Jean-Luc Barrière that made this bridge possible.

**YeneY is the default UPnP layer**, tested on Study, a Sonos Play:1.
`SONOS_LMS_UPNP=noson` selects the original noson backend; explicit
`SONOS_LMS_UPNP=yeney` selects YeneY. `own` remains a permanent alias for YeneY.
Selection is read and logged once at startup. For example:

```sh
SONOS_LMS_UPNP=yeney ./sonos-lms --room="Sonos Port" --server=192.0.2.10
```

For a service, set `Environment=SONOS_LMS_UPNP=noson` in its systemd override
to use the original backend. `--list-rooms` follows the same selection, as does
`scripts/device-test.sh` room discovery, which retries with noson if the selected
YeneY discovery fails.
The YeneY layer discovers speakers with SSDP and keeps one speaker-state snapshot from
AVTransport, RenderingControl and ZoneGroupTopology events. AVTransport commands
and reads target the group coordinator; volume and topology target the room's own
speaker. Bridging a member room therefore controls its group, as noson does. The
YeneY HTTP server handles the stream, icon and GENA events on one port without
constructing or initializing noson. The noson selection keeps its original server.
YeneY accepts up to 16 simultaneous connections; excess connections receive 503.
Headers are limited to 16 KiB and five seconds. The diagnostic `--file` mode
requires the noson backend; YeneY serves only the registered stream, icon and
GENA paths.

**Events (GENA).** One listener accepts `/avt`, `/rc` and `/zgt`. Subscriptions
request 300 seconds, renew halfway through the granted lifetime, and retry after
one second, then every five seconds. Each renewal checks the local address towards
the speaker; an address change creates a new subscription. Coordinator changes
move only AVTransport. The listener shares the HTTP stream port on `0.0.0.0`, selecting the first
available port from 1400 through 1409. `SONOS_LMS_EVENT_PORT` has been removed;
use the stream URL port for packet captures.
The first NOTIFY body after each subscription or renewal is logged per service,
on one line, truncated to 4 KB, to collect real device fixtures.

With all three subscriptions active, the YeneY layer polls only position while PLAYING or
TRANSITIONING (at most once per second), plus a transport sanity check every
30 seconds. A missing subscription enables polling only for that service until
it recovers. Logs report `yeney: monitor events` or the affected polling service
and reason. A delayed poll cannot overwrite fields updated by an event after that
request began. Metadata-only events preserve other fields.

For A/B testing, `SONOS_LMS_YENEY_POLL=legacy` restores the previous polling
schedule; the default is `events`. `SONOS_LMS_YENEY_STOPPED_MEDIAINFO=0` (default)
skips fallback/legacy periodic GetMediaInfo while STOPPED or PAUSED_PLAYBACK.
Setting it to `1` restores the earlier behavior, which still skips that read while
a paused stream request is open. Explicit URI checks are unaffected. Both modes
are read and logged at startup. These polling settings do not change pause/stop
ordering, resume decisions or action timeouts.

The parser fixtures include captured Sonos AVTransport, RenderingControl and
ZoneGroupTopology notifications. The latter two cover Master volume 26 and
three solo rooms: Study and MBR on firmware 86.10, and Sonos Port on 97.1.

See [the UPnP layer inventory and wire fixtures](docs/upnp-layer.md). No SMAPI
library service or external-playback ownership policy is enabled by this switch.

## Related

- [philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP) -- the mature,
  general-purpose LMS-to-UPnP bridge; see the comparison at the top.
- [janbar/noson](https://github.com/janbar/noson) -- the Sonos library by
  Jean-Luc Barrière that YeneY started on and is gradually replacing.
- [ralph-irving/squeezelite](https://github.com/ralph-irving/squeezelite) --
  the LMS player YeneY is built on.

## License

Project-owned code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE)
(`PolyForm-Noncommercial-1.0.0`). See the license for permitted purposes and terms.

Third-party code retains its existing licenses, including the GPL-3.0 licenses
in [noson/LICENSE](noson/LICENSE) and [squeezelite/LICENSE.txt](squeezelite/LICENSE.txt).
This change does not relicense third-party code or revoke rights granted by
previous GPL releases.

The current executable links GPL-covered dependencies. PolyForm Noncommercial's
noncommercial restriction is incompatible with distributing that combined work
under the GPL. Distributing a combined executable requires resolving this
conflict, for example through separate permission from the relevant copyright
holders or replacement of the GPL-covered code. See the
[GNU GPL FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#GPLIncompatibleLibs).

Copyright (C) 2026 Jaap van Vliet
