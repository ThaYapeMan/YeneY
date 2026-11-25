# Queue refill probe

Run this measurement from the YeneY checkout as root. It uses Python 3,
the checkout's WebSocket reader, systemd, Make, a C++ compiler, libFLAC, and the OpenSSL development library (libcrypto).
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
| `packed16-4096`, `packed16-1024`, `packed16-256` | Packed 16-bit FLAC, block size in the chain name |
| `packed24-256` | Packed 24-bit FLAC, 256 samples per block |
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


## Listening for music

The refill and seek sets measure position and requests. They cannot tell
whether a row was audible. Use `--set listen` when someone in the room can
hear the speaker and answer the terminal prompts.

On 23 November 2026, a Play:1 running software 86.11 was silent on the
bridge's verbatim 16-bit and 24-bit FLAC queue rows. The speaker reported
playback, advanced its position, fetched the whole file and reported no
error. WAV rows were audible. On 24 November, with the 44.1 kHz, 16-bit
track 53454, the probe's WAV and libFLAC `packed16` rows were audible.
Its `fixed16` row with a seek table was silent. Both FLAC files used
4,096-sample blocks. The earlier position and request measurements had
classified the FLAC forms as playing without a fault.

Use the same speaker, room and LMS options as for the refill set. Add
`--set listen --check` to check prerequisites and prepare the files.
Remove `--check` for the listening run. The room must be stopped and
ungrouped with crossfade off and the volume audible. Keep other room
controls idle and stop LampaStream from following the room during the
run. The probe stops an active room service and restarts it during cleanup.
`--normal-play-mode` temporarily selects NORMAL and restores the original
mode afterward.

Each row contains 20 seconds of audio starting 60 seconds into the source.
The probe starts it from the beginning, lets it run for 15 seconds, stops
the speaker and asks whether you heard music. Answer `y` for heard,
`n` for silent or `r` to hear the same row again. The controlling
terminal accepts answers when output is piped through `tee`. The default
eight cases take about four minutes including time to answer. Use
`--cases` for a comma-separated subset in the desired order and
`--repeats` to run that sequence again. Automated tests can use
`--answers` with one `y` or `n` per case per repeat and optional `r`
entries before each answer.

The order is `wav`, `packed16`, `fixed16`, `fixed16-subset`,
`fixed16-md5`, `fixed16-subset-md5`, `fixed16-256-subset`, and
`fixed24-256-subset`. The six fixed cases use verbatim frames without a
seek table. The sample rate and bit depth are now written in frame
headers by default. The listen cases `fixed16` and `fixed16-md5` pass
`--legacy-header` to preserve the original form that refers to STREAMINFO
for these fields. The listen set keeps the same eight files byte for
byte. The MD5 switch writes the checksum of the interleaved PCM into
STREAMINFO. The probe verifies decoded PCM and the frame-offset suffix,
and checks the first and last frame headers for the subset cases.

On 25 November, the owner heard the fixed rows with explicit frame
headers on the Study Play:1, both with and without MD5. The two legacy
header controls were silent. The measurements of fixed rows from
21 to 24 November were made on silent rows. From PROBE-05 onward, fixed
rows in the refill and seek sets use explicit frame headers.

The result table records the answer, replays, transport state, reported
WebSocket quality, playback errors and HTTP requests. The closing line
reports the controls and paired changes in answers. If either control
was silent, check the volume and room before drawing conclusions.
Keep the printed archive path and your run log. The archive contains
`answers.json` alongside the raw records and summary.


## Listening to mixed queue rows

Use `--set mix --normal-play-mode` with a native 16-bit FLAC track of
at least 120 s. The default set takes about eight minutes plus time to
answer and replay. Use `--cases` and `--repeats` to select and repeat
cases. The terminal asks one question at a time after stopping the
speaker. Answer `y` for yes or `n` for no. At the first question, `r`
replays the case from the beginning. Tests can pass these responses in
order with `--answers`.

| Cases | What happens |
| --- | --- |
| `wav`, `fixed16-256`, `packed16-256`, `packed24-256` | A single row runs for 15 s. The WAV control is 24-bit. |
| `wav-wav`, `fixed-fixed`, `packed-packed`, `fixed-packed`, `packed-fixed` | Two contiguous 20 s cuts run until 10 s into the second row. |
| `packed-nosize`, `packed-fixedsize`, `fixed-packed-nosize` | The packed row's description omits size or gives the corresponding fixed file's size. HTTP responses give the true file length throughout. |
| `fixed-seek`, `packed-seek` | Play 6 s, seek to 20 s, then play 6 s. |
| `fixed-pause`, `packed-pause` | Play 8 s, pause 5 s, then resume for 8 s. |

The cuts start 60 s into the track. FLAC rows use blocks of 256 samples.
Fixed rows state the rate and bit depth in each frame and omit a seek
table. Packed rows use libFLAC and include its seek table. The joins and
other cases use 16-bit rows except for the plain 24-bit WAV and packed
24-bit controls.

The archive contains the answers in `answers.json` and the speaker's
transport state, WebSocket quality, errors and HTTP requests in the
summary and raw records. The HTTP counts are bytes written by the
server. For joins, `join_ms` is positive when the beginning of the second
row falls after the end predicted by the first row's reported position.
The archive also marks requests after each seek and records positions
before pausing and just after resuming. Only the listener's answers can
establish audibility. If either WAV control fails, the closing line asks
the listener to check the volume and room. Keep the archive and the run
log together.

The imitation speaker accepts descriptions without size as a test
assumption. That behavior was not derived from a speaker capture.

Runs that prepare packed files, and every mix run, print a 16-bit size
table for seconds 60 through 120 of the test track. It compares packed
FLAC at block sizes 256, 1,024 and 4,096, fixed FLAC at 256 and 16-bit
WAV, using stereo PCM for all five entries. The table gives the actual
file lengths and each as a percentage of the WAV length.
