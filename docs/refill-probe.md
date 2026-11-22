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

## Seek chains

Use `--set seek` to measure how far the reported position and the audible
row join diverge after a seek. Each chain uses eleven contiguous 40 s
segments from one FLAC track. Provide at least 440 s of native 44.1 kHz
audio. The default chains require a 16-bit source. Use `--track-id-48`
with a separate 16-bit 48 kHz FLAC of the same minimum length to include
the optional 48 kHz chain. The probe verifies the source rate and never
resamples. Use `--cases` and `--repeats` within the selected set.

| Chain | Audio |
| --- | --- |
| `wav` | 24-bit WAV |
| `fixed16-4096`, `fixed16-1024`, `fixed16-256` | Fixed-size 16-bit FLAC, block size in the chain name |
| `fixed24-256` | Fixed-size 24-bit FLAC, 256 samples per block |
| `packed16-4096`, `packed16-1024` | Packed 16-bit FLAC, block size in the chain name |
| `fixed16-4096-48k` | Optional fixed-size 16-bit FLAC at 48 kHz |
| `fixed16-4096-ms` | Unavailable pending verification of the Sonos WebSocket seek reference |

The first row starts with Play and has no action. After 4 s, rows 2–7
seek to 20, 21, 22, 23, 24, and 26 s, respectively. Row 8 seeks to 20 s
and then to 27 s three seconds later. Row 9 seeks to 24 s, plays for 2 s,
pauses for 5 s, then resumes. Row 10 plays for 20 s and seeks backwards
to 2 s. Each row ends naturally. The probe measures into the following
row and plays 6 s of the closing row. One chain takes about 4½ minutes.

The seek table's `join_ms` is positive when the next row starts early
relative to the previous row's reported clock. The default refill table
retains its earlier `join_gap_ms` sign, where a positive result means a
late start. Seek joins use the eight nearest playing WebSocket answers
on each side, excluding round trips of 30 ms or longer. For FLAC, the
prediction is one block plus the samples to the next boundary, or
one block when the seek lands on a boundary. With two seeks the
prediction uses the last target. WAV and the control row predict zero.
The summary reports the largest absolute residual across seek rows and
the largest absolute control join. A complete chain within 3 ms gives
`H holds`. A residual beyond 3 ms gives `H fails`. The measurement
compares the position clock with the row boundary and cannot establish
which is out of step with the sound.

Fixed-size seek FLAC rows omit seek tables. Preparation computes their
byte count and an interior frame offset directly from the sample count,
block size and sample format. It verifies the full PCM and the PCM decoded
starting at that offset. Packed FLAC byte counts depend on the sample
values. For those rows preparation verifies the decoded frame boundaries,
seek table and PCM, including decoding at an independently read frame
offset. The archive includes a chain, repeat and row for each measured
playback window and media request.

The LMS download allows up to 120 s in total and prints
`waiting for LMS to deliver the track` once after 10 s.
