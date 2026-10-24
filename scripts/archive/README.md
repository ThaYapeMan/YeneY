# Finished owner experiments

Archived by owner decision on 24 October 2026. These are historical references,
not installed tools or active acceptance tests. Use the maintained
`/usr/local/bin/yeney-queue-device-suite` for queue device qualification.

- `stream-probe.py`: could Sonos show changing stream metadata without the bridge? ICY in-band metadata was rejected; see [CLAUDE.md, Rejected approaches](../../CLAUDE.md#rejected-approaches) and [metadata findings](../../docs/how-it-works.md). Queue items now carry per-track details.
- `reference-relay.py`: how does Sonos consume and resume a live relay? Use `YENEY_PAUSE=stop`; see [pause findings](../../docs/how-it-works.md) and [settings](../../README.md#settings).
- `reference-test.sh`: confirmed that live-stream pause needs Stop followed by a fresh start; the same pause conclusion and evidence live in [how it works](../../docs/how-it-works.md).
- `replaygain_probe.py`: completed the one-off offline S9 ReplayGain measurement; methodology and result criteria remain in [device verification](../../DEVICE-VERIFICATION.md). The kept device-test command can still invoke this archived helper explicitly.
- `sonos-queue-probe`: the 12 October queue experiment proved AddURIToQueue; see [probe conclusions](../../docs/designs/sonos-queue-probe.md). Keep its exact device-proven request as a reference; the device suite supersedes the experiment.
- `migrate-from-sonos-lms.sh`: completed the one-time name/configuration/service migration; historical migration instructions remain in Git history at `c3baa3d:README.md`. Normal installs now use the YeneY installer.

Their dedicated tests are retained under `tests/archive/` but are not part of
`make test`. Historical scripts retain their original behaviour; they are not
permission to run device experiments from an agent session.

Live legacy-setting startup checks formerly mixed into the migration test remain
active in `tests/obsolete_player_test.py`; only migration coverage is archived.
