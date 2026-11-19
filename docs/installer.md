# Installer details

Moved from the README.

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
9090, waiting at most 15 seconds for the MAC from the room status with `connected:1`.
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
