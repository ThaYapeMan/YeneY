# How YeneY works

Technical background for [YeneY](../README.md): the design, the Sonos behaviour it works around, and the test tooling. For installation and settings, see the README.

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
FLAC encoder and opens an HTTP endpoint (`/music/yeney.flac`) that Sonos is
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

The [comparison table in the README](../README.md#yeney-compared-with-lms-upnp) shows the trade-off for YeneY and both LMS-uPnP
modes.

**In short:** a track-by-track approach mainly improves things *in the Sonos
app*. If you control playback from LMS -- Material Skin, the web interface,
iPeng -- what you gain is limited, and what you give up (LMS sound processing and
synchronisation) is real. YeneY deliberately chooses the continuous stream and
treats the Sonos as a real LMS player.

The continuous item has radio-style controls, but the CurrentURI scheme selects
which Sonos playback client handles it. In the 28 September device probes,
`x-rincon-mp3radio://` selected the radio client and requested ICY for both MP3
and FLAC; `http://` selected the normal HTTP client and never requested ICY.
This is a URI-scheme distinction, not an MP3/AAC-versus-FLAC rule. See
[Where it falls short](#where-it-falls-short) for the playback and app results.
Status:

- **Track titles -- work in progress.** Starting a new stream at each track change
  updates the title but can introduce a gap, unsuitable for gapless albums or DJ
  mixes. In the probes, MP3 ICY titles reached `r:streamContent`, but the current
  Sonos app still showed only the title; ICY support alone did not solve app updates.
- **Next/Previous and seeking in the Sonos app -- under investigation.** Not
  possible while the Sonos sees a radio stream. The idea being explored: give the
  Sonos one item per track that still points to this bridge, so LMS keeps producing
  the audio (with all its processing) while the Sonos app gains Next, Previous and a
  seek bar. Whether Sonos accepts this without new pause or gapless problems still
  has to be proven on real speakers.

## Audio quality

| Setting | Values | Default |
|---|---|---|
| `YENEY_AUDIO` | `24/48`: 24-bit FLAC at the source's 44.1/48 kHz rate; `16/44`: legacy 16-bit/44.1 kHz | `24/48` |

The setting is read once at startup and logged. Invalid values warn and use
`24/48`. Restart the bridge after changing it, for example with a systemd
`Environment=YENEY_AUDIO=16/44` override for comparison or older speakers.
The default targets Sonos S2's 24-bit/48 kHz FLAC support.

At 44.1 and 48 kHz, decoded PCM keeps its sample rate and up to 24 bits of
precision. A 16-bit source is padded with zero bits, without changing its sample
values. LMS ReplayGain and fade-in, fade-out and fade-in-out are applied in both
audio modes. LMS volume is not applied to PCM; Sonos volume remains independent.
Bit-exact playback assumes LMS ReplayGain, fades, DSP, crossfade and other sample
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

## Where it falls short

**Artist in the Sonos app.** Device tests on 28 September 2026 (Study, Play:1)
showed that the app displays only `dc:title` for radio-type `x-rincon-mp3radio`
items. It ignored `dc:creator`, `upnp:album`, structured and plain
`r:streamContent`, `r:radioShowMd`, and changing the class to musicTrack or
audioBroadcast. Putting the artist in `dc:title` made it visible.

`YENEY_TITLE_FORMAT` controls that title:

- `artist-title` (default): `<artist> - <title>`, or just `<title>` if artist is empty.
- `title`: `<title>`, the previous display format.

The setting is read and logged once at startup; invalid values warn and select
`artist-title`. The status table, bridge log and controller's sent-title cache
use the same formatted title. Text is XML-escaped. Artist and album remain in
`dc:creator` and `upnp:album` for other controllers, including Home Assistant;
empty fields are omitted.

`YENEY_STREAM_CONTENT` independently controls `r:streamContent`, using the
original track title, artist and album:

- `structured` (default): `TYPE=SNG|TITLE <title>|ARTIST <artist>|ALBUM <album>`.
  Empty fields are omitted and `|` inside a value becomes `/`.
- `plain`: `<artist> - <title>`, or just `<title>` when artist is empty.
- `off`: empty, preserving the earlier radio-text behavior.

It is read and logged once at startup. Invalid values warn and use `structured`.
Changing the title format does not change these radio-text fields.

`scripts/meta-variant.py` is a device-test aid. While the speaker is playing the
bridge stream, run from a host that can reach its coordinator:

```sh
python3 scripts/meta-variant.py 192.0.2.145 show
python3 scripts/meta-variant.py 192.0.2.145 original
python3 scripts/meta-variant.py 192.0.2.145 title noplay
```

`show` only reads metadata; `original` resends it unchanged. The variants `title`,
`show-md`, `track`, `broadcast` and `album` modify one metadata field, then send
SetAVTransportURI and Play with the same stream URL, without issuing LMS commands.
The optional final `noplay` argument omits Play: it sends only SetAVTransportURI
to test whether metadata refreshes without restarting playback. After either
form, the diagnostic reads and prints transport state four times at two-second
intervals. `show` remains read-only and returns immediately.
Inspect the Sonos app after each run. For a clean `title` experiment, start the
bridge with `YENEY_TITLE_FORMAT=title` and start a fresh LMS stream first:
the diagnostic deliberately prefixes the current title without deduplication.
Restart the LMS stream to restore the bridge's configured metadata.

`scripts/stream-probe.py` is a separate device-test aid that serves an audio source
from this host on port 18080 and points the speaker at it without using the bridge.
Stop the room's bridge service before running it, then restart that service when
finished. For example, on the bridge host:

```sh
sudo systemctl stop 'yeney@Study'
python3 scripts/stream-probe.py 192.0.2.145 flac-radio 'http://<lms>:9000/music/<id>/download'
sudo systemctl start 'yeney@Study'
```

Replace the source URL with a reachable audio file or stream; the script's default
is a site-specific LMS track. `mp3-radio` requires an explicit MP3 source URL.
The other variants are `flac-radio-mpeg`, `flac-radio-force`, `flac-radio-noicy`
and `flac-track`; run without arguments to print their descriptions. The probe
logs stream request headers and ICY opt-in, changes ICY titles every 15 seconds
when enabled, and samples the speaker's stored metadata and position every five
seconds, printing changes. Ctrl+C stops the speaker and the probe server.

**URI scheme, ICY and FLAC playback.** Device probes on 28 September 2026 with
`scripts/stream-probe.py` showed that a CurrentURI beginning with
`x-rincon-mp3radio://` selects Sonos's radio client. It sends `Icy-MetaData: 1`
and a User-Agent ending in `Nullsoft Winamp3` for MP3 and FLAC alike. A CurrentURI
beginning with `http://` uses the normal HTTP client and never requested ICY in
these probes. The request depends on the URI scheme, not simply the audio codec.

The radio client buffered and reconnected instead of playing FLAC. MP3 played,
and its ICY `StreamTitle` reached `r:streamContent`, but the current Sonos app
still displayed only the title. Therefore ICY delivery and Sonos-app title
display are separate observations.

YeneY deliberately keeps FLAC's CurrentURI as `http://` while advertising
`x-rincon-mp3radio:*:audio/flac:*` in DIDL `protocolInfo`. This combination keeps
the normal HTTP client, which plays FLAC. Changing CurrentURI to
`x-rincon-mp3radio://` breaks FLAC playback; the radio protocolInfo alone does
not switch clients. This wire behaviour is unchanged.

**In-stream metadata updates don't work in the bridge.** The normal HTTP client
used by the bridge does not opt into ICY. Injecting ICY blocks into that FLAC
response anyway corrupts playback (`ERROR_CORRUPT_FILE`). Track title and artwork
are still set only at stream start. Switching to the radio URI is not a FLAC
metadata workaround, and the MP3 probe did not make the app display StreamTitle.

Stream URLs include a random token generated once per process start:
`/music/yeney.flac?session=<token>&stream=<N>`. The startup log prints
`Stream session: <token>`. GET and HEAD requests with a missing or different
token receive an empty 404 with `Connection: close`, before encoder ownership
or device-resume handling. Old URLs cannot match a reused stream ID after restart.

## Pause and resume on Sonos

Pausing ends the HTTP response and sends UPnP **Stop** by default. Sonos resumes
FLAC with radio protocolInfo (`x-rincon-mp3radio:*:audio/flac:*` and an HTTP URI)
incorrectly from PAUSED:
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
No environment overrides are needed. `YENEY_PAUSE=pause` remains an
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

YeneY owns discovery, SOAP control, event subscriptions, HTTP serving and the
encoded-packet buffer. It is the only UPnP layer. `YENEY_UPNP=yeney` and the
permanent alias `own` both select it; leaving the setting unset does too.
Any other value warns `YENEY_UPNP=<value> is no longer supported; using YeneY`
and continues. Startup retains `UPnP layer: yeney`, with ` (alias own)` for `own`.
The setting is read and logged once, including for `--list-rooms`.

The layer discovers speakers with SSDP and keeps one speaker-state snapshot from
AVTransport, RenderingControl and ZoneGroupTopology events. AVTransport commands
and reads target the group coordinator; volume and topology target the room's own
speaker. Bridging a member room therefore controls its group. Room discovery in
`scripts/device-test.sh` reports failures directly.

The HTTP server handles the stream, icon and GENA events on one port. It accepts
up to 16 simultaneous connections; excess connections receive 503. Headers are
limited to 16 KiB and five seconds. Only the registered stream, icon and GENA
paths are served. The encoded buffer retains 256 packet slots and the established
overwrite/read order. Its first overflow in each stream logs
`encoded buffer: overwrote oldest packet, capacity 256 packets`; reconnects do
not repeat that warning for the same stream.

Device evidence on `0a210f2`: Study (Play:1) passed S2, S5, S6 and five S7 runs;
Sonos Port passed S1, S2, S5, S6 and S7; Study grouped under MBR passed S1 and S7.
The one S1 failure followed LMS dissolving a sync group with a Squeezebox Radio
and sending `strm q`.

**Events (GENA).** One listener accepts `/avt`, `/rc` and `/zgt`. Subscriptions
request 300 seconds, renew halfway through the granted lifetime, and retry after
one second, then every five seconds. Each renewal checks the local address towards
the speaker; an address change creates a new subscription. Coordinator changes
move only AVTransport. The listener shares the HTTP stream port on `0.0.0.0`, selecting the first
available port from 1400 through 1409. Use the stream URL port for packet captures.
The first NOTIFY body after each subscription or renewal is logged per service,
on one line, truncated to 4 KB, to collect real device fixtures.

With all three subscriptions active, the YeneY layer polls only position while PLAYING or
TRANSITIONING (at most once per second), plus a transport sanity check every
30 seconds. A missing subscription enables polling only for that service until
it recovers. Logs report `yeney: monitor events` or the affected polling service
and reason. A delayed poll cannot overwrite fields updated by an event after that
request began. Metadata-only events preserve other fields.

For A/B testing, `YENEY_POLL=legacy` restores the previous polling
schedule; the default is `events`. `YENEY_STOPPED_MEDIAINFO=0` (default)
skips fallback/legacy periodic GetMediaInfo while STOPPED or PAUSED_PLAYBACK.
Setting it to `1` restores the earlier behavior, which still skips that read while
a paused stream request is open. Explicit URI checks are unaffected. Both modes
are read and logged at startup. These polling settings do not change pause/stop
ordering, resume decisions or action timeouts.

The parser fixtures include captured Sonos AVTransport, RenderingControl and
ZoneGroupTopology notifications. The latter two cover Master volume 26 and
three solo rooms: Study and MBR on firmware 86.10, and Sonos Port on 97.1.

See [the UPnP layer and wire fixtures](upnp-layer.md). No SMAPI library
service or external-playback ownership policy is implemented.

## Testing and device-test tooling

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
on a real speaker. Use the levels below and [DEVICE-VERIFICATION.md](../DEVICE-VERIFICATION.md)
to choose the physical checks appropriate to a change.

### Device test levels

- **Level 0 — no physical test:** display, logging and installer changes. Run `make` and `make test` locally.
- **Level 1 — unattended:** for stream and pause logic changes, run `sudo env AUTO=1 scripts/device-test.sh` on the deployment host. It discovers the room's coordinator, sends AVTransport Pause/Play directly, allows up to ten seconds after resume for both playback clocks to start, then checks speaker and LMS time advance by at least three seconds within the next six seconds, checks continued position and track changes, and monitors transport status and journal errors. It prints a PASS/FAIL table and exits nonzero if any scenario fails. This measures playback progress; it cannot hear audio or inspect app dialogs.
- **Level 2 — real Sonos app:** occasionally run `sudo scripts/device-test.sh` and follow the existing app prompts, especially to confirm audible playback and app behavior. `QUICK=1` keeps this manual flow with shorter defaults.

AUTO and QUICK default to `SCENARIOS="1 2 5 6 7" S2_ROUNDS=1 LONG_PAUSE=30`;
explicit environment values override these defaults. Normal manual mode retains
scenarios 1–7, three S2 rounds and a 120-second long pause. AUTO requires Python 3,
`./yeney --list-rooms --details`, coordinator reachability on port 1400, LMS CLI
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
