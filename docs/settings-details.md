# Settings in detail

Moved from the README. The table of settings is in the [README](../README.md#settings).

## Player name

The true player name is always `<room> (Sonos)`, alongside the unchanged room
name and MAC. Only the display name sent to LMS changes. Precedence is
`--show-sonos-suffix=yes|no`, then the per-room process environment
`YENEY_SHOW_SONOS_SUFFIX`, then the global `YENEY_SHOW_SONOS_SUFFIX=yes|no`
key in `/etc/yeney/config` (`YENEY_CONFIG` selects another file), then `yes`.
An invalid selected value warns once and uses `yes`. This also works directly:

```sh
./yeney --room=Study --server=192.0.2.23 --show-sonos-suffix=no
```

LMS has one player-name field, so every LMS interface shows the display name.
The MAC-bound player and its preferences/playlist remain the same. A rename in
LMS lasts until the next YeneY restart, when the configured display name returns
through SlimProto SETD. Startup logs show both names and the setting source;
the room status publishes `true_name`, `display_name` and `mac`. Tools select by
MAC and use the true name for reporting, independently of the LMS display name.

## Volume

Volume control also works when starting the binary directly:

```sh
YENEY_VOLUME_MAX=70 ./yeney --room=Study --server=192.0.2.23 --lms-volume=speaker
```

Volume mode precedence is `--lms-volume=off|speaker`, then the room process's
`YENEY_LMS_VOLUME`, then `room.Study.lms_volume=off|speaker` in
`/etc/yeney/config`, then `off`. The cap uses `YENEY_VOLUME_MAX`, then
`room.Study.volume_max=70` in that same config, then 100. `YENEY_CONFIG` can
select another volume configuration file. Invalid mode values warn once and
select off; invalid caps warn once and select 100. Effective mode, source and
cap appear in the startup log and room status (`lms_volume`,
`lms_volume_source`, `volume_max` inside `playback`). Restart after changing
settings. Config keys are suitable for a future per-player LMS plugin.

LMS 9.1.1 does **not** offer its native fixed/adjustable volume preference for
YeneY's player model: **Settings > Player > select the YeneY room > Audio** has
no Volume control selector. The server's `digitalVolumeControl` preference
exists internally, but YeneY does not advertise `HasDigitalOut`; LMS hides the
selector and enforces adjustable internally. YeneY never changes that preference.
The YeneY setting therefore controls this feature.

In speaker mode LMS's exact integer mixer volume (not its nonlinear SlimProto
gain) sets the room's own Sonos volume. A coordinator with members snapshots the group
volume ratios before using group volume, and uses group mute; a standalone room or member uses its own volume/mute.
App-side changes reach LMS through its CLI with echo suppression. Changes settle
for 200 ms, and SOAP writes are spaced at least 200 ms apart across volume and
mute; unchanged values are skipped. Own changes await confirmation for up to
two seconds, suppressing intermediate group feedback. A separate bounded worker queries LMS and
confirms speaker state; RenderingControl and GroupRenderingControl events wake
it immediately, with a 100 ms polling fallback. A capped LMS value does not
rewrite the LMS slider. The cap limits YeneY's writes; the Sonos app can still
set its own volume above it. Both modes preserve audio samples without PCM
volume scaling. Off mode starts no volume worker and sends no mixer commands
or volume/mute writes.


## Artwork

Artwork in both stream and queue mode comes from LMS. Local library tracks keep
`http://<lms>:9000/music/<id>/cover.jpg`. Remote tracks (podcasts, internet radio
and streaming services) use LMS's `artwork_url`: relative paths use the LMS HTTP
host, and absolute HTTP/HTTPS images go through its image proxy so Sonos fetches
plain HTTP from LMS. Existing image-proxy paths retain their inner percent
escapes. Remote tracks without artwork send no album-art element. Stream
metadata is fetched at stream start; this adds no midstream polling.

## Queue mode

Queue mode is the default. It shows the whole LMS playlist in the Sonos queue up to
`YENEY_QUEUE_ROWS` rows; Next, Previous, seeking and a tap on a row in the Sonos app reach LMS.

Settings apply on room service restart. Queue is built in; existing owner
`Environment=YENEY_PLAYBACK_MODE=queue` overrides are redundant but harmless and
are never removed by the installer. Explicit `stream` remains supported; `auto`
and invalid settings still select stream and log the effective mode. At every
new track, live topology must identify this room as coordinator. Adding or
removing members does not change its per-source path. A member of another group
writes no transport commands and reports one LMS stop, rather than controlling
the other room or walking the playlist.

Queue mode owns only its prepared finite WAV entries. Local files retain exact
promises. Remote flags, negative IDs and non-file URLs select `remote-finite`
relay when LMS supplies a positive non-live duration; live and unknown-duration
sources select the existing continuous stream path. Both paths start
progressively without waiting for the whole source. Class changes wait for the
outgoing track to finish, then start the other path; a short measured gap is
logged. Diagnostics expose `path=relay|stream` and
`reason=local-source|remote-finite|remote-source`. Pacing logs and socket-wait accumulation
are suppressed on the relay path and unchanged on the stream path. LMS controls play, pause, resume, stop and source
seeks; confirmed speaker starts advance LMS through SlimProto. Queue mode reports unambiguous speaker-originated stops and pause/resume to LMS
through one logged CLI mutation path. Normal LMS commands never echo as CLI mutations; a confirmed room fault or
member refusal reports one explicit LMS stop. Source changes release owned entries without stopping the new source.
There is no automatic fallback in this relay release. Source failures report STMn; room-level faults stop LMS once without skipping tracks.

Owners can qualify a standalone room or an owner-formed group coordinator with
`/usr/local/bin/yeney-queue-device-suite --room Study --server 192.168.178.23`,
optionally `--album <id-or-name> --volume 10 --podcast <episode-or-menu-name>`. The suite saves/restores volume,
LMS state and room configuration; it checks album joins, LMS pause/resume and
stop/new play, and a remote favourite/podcast when available, followed by a
local album returning to the relay. Missing remote entries produce a documented
skip. Use the [systemd-run procedure](../DEVICE-VERIFICATION.md#phase-3c-unattended-queue-playback-device-suite)
to survive an SSH disconnect. [Relay status and limitations](designs/sonos-queue-mode.md)
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
[device verification](../DEVICE-VERIFICATION.md#unattended-owner-stream-pacing-qualification).

## LMS player icon (Material Skin)

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
`yeney-queue-suite` (its isolated `$TMPDIR`, or `/var/tmp`, workspace). Those tools do not read the
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

## Older debug setting names

The old `YENEY_DEBUG_EVENTS=1`, `YENEY_DEBUG_STREAM=1`, `YENEY_POLL=legacy`
and `YENEY_STOPPED_MEDIAINFO=1` names still work, with one deprecation warning
each at startup. Canonical debug tokens enable the selected experiments; an
enabled legacy alias also keeps its previous effect. All timing/LampaStream and
user-facing playback settings retain their names and defaults.


## Queue audio format

`YENEY_QUEUE_FORMAT` selects `flac` (default) or `wav` for exact-length
rows. Each room reads its process environment, its
`room.<name>.YENEY_QUEUE_FORMAT` setting, then the global
`YENEY_QUEUE_FORMAT` setting in `/etc/yeney/config` at startup. Invalid
values log a warning and use FLAC.

Run `yeney-format` to list the enabled rooms and their configured choices.
Run `sudo yeney-format wav "Sonos Port"` to switch one room, or
`sudo yeney-format flac all` to switch all enabled rooms. The command saves
the room settings, restarts the selected services and prints each startup
format line.

FLAC can use 16 bits for original files that YeneY decodes at proven
16-bit depth without sample processing. Other FLAC rows and all WAV rows
use 24 bits. Remote tracks and podcasts with a length margin use WAV in
either setting.
A playlist can contain both formats. The join between them has yet to be
measured on a speaker.

## Queue audio pacing

Each room reads these settings at startup. They apply to queue HTTP bodies,
including remote tracks and podcasts. Invalid values log a warning and use
the corresponding default.

| Setting | Values | Default |
| --- | --- | --- |
| `YENEY_QUEUE_PACING` | `on` or `off` | `on` |
| `YENEY_QUEUE_PACE_RATE` | 1.0–8.0 times the item's served byte rate | `2.0` |
| `YENEY_QUEUE_PACE_FREE_MS` | 0–30000 ms of audio available at once | `2000` |
| `YENEY_QUEUE_PACE_TAIL_MS` | 0–120000 ms at the end of an item | `0` |

Each request starts with the free allowance. The allowance refills at the
configured rate up to the same limit. Bytes in the configured tail are
delivered without a rate limit, including when a request starts earlier
and reaches the tail.
