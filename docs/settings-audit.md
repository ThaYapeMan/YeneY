# YeneY settings audit (24 October 2026)

Active room settings and compatibility readers; test-runner file/path variables are not room settings.
No active configuration variable has an unreachable implementation. `YENEY_PLAYER`
is only a compatibility warning, not an engine selector, and its existing warning is retained.

| Name | Reader | Still used | Decision |
|---|---|---|---|
| `YENEY_AUDIBLE_OFFSET_MS` | `timing_settings.h` | yes | Kept unchanged |
| `YENEY_AUDIO` | `audio_mode.h` | yes | Kept unchanged |
| `YENEY_DEBUG` | `debug_settings.h` | yes | Kept: new canonical debug list |
| `YENEY_DEBUG_EVENTS` | `debug_settings.h`, `upnp/own_speaker_control.cpp` | yes | Merged into YENEY_DEBUG; old alias retained with startup deprecation |
| `YENEY_DEBUG_STREAM` | `debug_settings.h`, `stream_debug.h` | yes | Merged into YENEY_DEBUG; old alias retained with startup deprecation |
| `YENEY_LMS_ICON` | `lms_icon.h` | yes | Kept unchanged |
| `YENEY_LMS_POSITION_FROM_MODEL` | `timing_settings.h` | yes | Kept unchanged |
| `YENEY_PAUSE` | `pause_mode.h` | yes | Kept unchanged |
| `YENEY_PLAYBACK_MODE` | `yeney.cpp` | yes | Kept unchanged |
| `YENEY_PLAYER` | `obsolete_player.h` | yes (warning only) | Kept compatibility warning only; no player-selection path |
| `YENEY_POLL` | `debug_settings.h`, `upnp/own_speaker_control.cpp` | yes | Merged into YENEY_DEBUG; old alias retained with startup deprecation |
| `YENEY_QUEUE_DISK_MIB` | `queue_item_store.cpp`, `queue_item_store.h` | yes | Kept unchanged |
| `YENEY_QUEUE_NEXT_DELAY_S` | `queue_protocol.h` | yes | Kept unchanged |
| `YENEY_QUEUE_POLL_MS` | `queue_room.cpp` | yes | Kept unchanged |
| `YENEY_START_LEAD_MS` | `start_lead.h` | yes | Kept unchanged |
| `YENEY_STOPPED_MEDIAINFO` | `debug_settings.h`, `upnp/own_speaker_control.cpp` | yes | Merged into YENEY_DEBUG; old alias retained with startup deprecation |
| `YENEY_STREAM_CONTENT` | `upnp/stream_content.h` | yes | Kept unchanged |
| `YENEY_STREAM_PACING` | `stream_pacing.h` | yes | Kept unchanged |
| `YENEY_TIMING_LOCKED_EVERY_S` | `timing_settings.h` | yes | Kept unchanged |
| `YENEY_TIMING_PRIOR` | `timing_settings.h` | yes | Kept unchanged |
| `YENEY_TIMING_PROBE` | `timing_probe_runtime.h` | yes | Kept unchanged |
| `YENEY_TIMING_PUBLISH` | `timing_settings.h` | yes | Kept unchanged |
| `YENEY_TIMING_RAW` | `timing_probe_runtime.h` | yes | Kept unchanged |
| `YENEY_TIMING_STALE_S` | `timing_probe_runtime.h` | yes | Kept unchanged |
| `YENEY_TITLE_FORMAT` | `upnp/title_format.h` | yes | Kept unchanged |
| `YENEY_UPNP` | `upnp/backend.h` | yes | Kept unchanged |

Only pure diagnostics/polling experiments were consolidated. Protected playback,
queue, metadata, pacing, backend and LampaStream timing settings are unchanged.
There are no runtime test-only environment hooks to merge; `YENEY_ACCEPTANCE_BINARY`
and `YENEY_REAL_LMS_*` configure the external acceptance harness rather than rooms
and remain unchanged so its test commands and scenarios remain the baseline ones.
