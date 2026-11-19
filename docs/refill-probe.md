# Study refill probe

Run this measurement from the YeneY checkout as root. It uses Python 3, the
checkout's WebSocket reader, systemd, Make, a C++ compiler, and the FLAC headers
and library used to build YeneY. Give it a stereo or mono FLAC track with at
least 180 s of audio. It builds the audio helper in a temporary directory.
Allow disk space for the LMS download, its stereo 24-bit WAV and the probe FLAC.

```sh
scripts/yeney-refill-probe --speaker 192.168.178.132 --room Study --lms-host 192.168.178.23 --track-id 123 --check
```

Replace 123 with the track's LMS ID. The check uses read-only speaker requests.
It confirms the local HTTP route and listener. The real speaker's ability to
connect back is established when it first requests a queue item.

Wait until the room is stopped and ungrouped. Run the same command without
`--check` to play. Leave LMS and the Sonos app alone for about 25 minutes. The
probe stops the Study bridge if it was running, temporarily appends two rows
per case, and restarts the bridge when it exits. Ctrl-C saves partial results.
Use `--volume` to change playback volume temporarily. Use `--cases` with a
comma-separated list or `--repeats` to shorten a follow-up run.

The WAV cases serve identical PCM. Two of them limit the transfer after
the first 2 s of audio. The FLAC cases use the track's compressed frames with
fresh stream metadata and one seek point per frame. The bounded case advertises
the WAV length and maps seek offsets onto FLAC frame boundaries.

The final line names the archive to attach. It contains timestamped requests,
speaker messages and measurements, per-case summaries, the log and versions.
The audio files are removed automatically.
