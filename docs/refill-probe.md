# Queue refill probe

Run this measurement from the YeneY checkout as root. It uses Python 3,
the checkout's WebSocket reader, systemd, Make, a C++ compiler, and libFLAC.
Supply a mono or stereo FLAC track with at least 180 s of audio. The default
set includes 16-bit cases and needs a 16-bit source. Allow room on disk for
the LMS download, the generated WAV and FLAC forms, and verification files.

```sh
scripts/yeney-refill-probe --speaker 192.168.178.132 --room Study --lms-host 192.168.178.23 --track-id 123 --check
```

Replace 123 with the LMS track ID. The check uses read-only speaker requests.
It reports the actual play mode and crossfade, verifies the advertised
control URLs and actions, and checks the local HTTP route and listener. The
real speaker's ability to connect back is established when it first requests
a queue item.

Wait until the room is stopped and ungrouped, with crossfade off. Run the
same command without `--check` to play. When the speaker has a different
play mode, add `--normal-play-mode` to select NORMAL after stopping the
bridge and restore the earlier mode before restarting it. Leave LMS and
the Sonos app alone while the measurement runs. Allow about 35 minutes for
the default 11 cases. Ctrl-C saves partial results. Use `--volume` to
change playback volume temporarily, `--cases` to choose a comma-separated
list, and `--repeats` to run them again. The default is one repeat.

Each case starts playback, seeks to 60 percent, pauses for 10 s, resumes,
then seeks to about 12 s before the end. The probe records the join and
observes the following row for 10 s. The short cases use three 30 s rows
and record both joins.

| Case | Row forms |
| --- | --- |
| `wav` | 24-bit WAV |
| `fixed16` | 16-bit verbatim FLAC |
| `fixed24` | 24-bit verbatim FLAC |
| `packed16` | 16-bit packed FLAC |
| `packed24` | 24-bit packed FLAC |
| `fixed-then-packed` | 16-bit verbatim FLAC followed by 16-bit packed FLAC |
| `packed-then-fixed` | 16-bit packed FLAC followed by 16-bit verbatim FLAC |
| `fixed16-noseek` | 16-bit verbatim FLAC without a seek table |
| `short` | Three 30 s WAV rows |
| `short-fixed16` | Three 30 s 16-bit verbatim FLAC rows |
| `bound` | 16-bit packed FLAC advertised with its 24-bit WAV upper bound |

Every GET starts with an allowance of 2 s of audio and refills that
allowance at twice the item's play rate. Requests starting within 20 s
of the end transfer at the receiver's rate. The bounded case keeps the
true FLAC byte offsets in its seek table and responses and reaches EOF
at the end of the encoded audio.

Verbatim FLAC uses 4096-sample blocks and no wasted bits. Preparation
checks the predicted file length and compares libFLAC's decoded PCM with
the source, both from the beginning and from a calculated frame offset.
Packed FLAC uses libFLAC level 5, blocks of 4096 samples, independent
stereo channels, encoder verification, and a seek table with about 100
true offsets. The 24-bit forms preserve the
source's top 16 bits and add a deterministic pattern in the low byte
at less than −110 dBFS.

The join gap uses the median playback-clock intercept on each side,
from the eight nearest millisecond position answers and their
request/response midpoints. The seek landing compares the first reported
playing position with the requested time. The per-case position offset
compares the steady playback-clock intercept with the first playing
answer. The summary also counts the audio requests per seek.

The final line names the archive to attach. It contains timestamped
requests, speaker messages and measurements, per-case summaries, the log
and versions. The generated audio files are removed automatically.
