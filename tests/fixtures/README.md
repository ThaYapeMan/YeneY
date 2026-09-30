# MP3 comparison fixtures

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

Copyright (c) 2026 Jaap van Vliet. Synthetic stereo signals from the pinned
core's `tests/decoders_test.py::signal_data`, 44,100 Hz, signed 16-bit:

- `lame-0.mp3`: 70,130 frames, starting at signal frame 0.
- `lame-70130.mp3`: 80,060 frames, starting at signal frame 70,130.

Encoded directly through libmp3lame 3.101 (Ubuntu package
`3.101~svn6525+dfsg-2`), 192 kbit/s, quality 2, with the encoder's genuine LAME
Info frame installed by `lame_get_lametag_frame`. Regenerate with:

```sh
python3 tests/fixtures/generate_mp3.py /path/to/libmp3lame.so.0
```

The test uses these committed files and needs no MP3 encoder installed. The
existing core fixtures were written by ffmpeg with a Lavc signature: core
honours their delay/padding fields, whereas the unchanged squeezelite fork
recognises only LAME. They therefore cannot establish equal gapless frame
counts between these engines. These fixtures exercise the shared LAME contract
without changing either decoder, rewriting metadata, or cropping/alignment of
output samples. General Lavc-tagged MP3 trimming remains different.

With float minimp3 output the fixtures measured 126.86/126.72 dB SNR against
libmad (previously 88.03 dB with int16 output), with correlations
0.999999999999924/0.999999999999920. Acceptance requires 120 dB SNR and
0.999999999999 correlation. A prefetched PCM marker releases the streaming FLAC encoder's partial block; its exact
boundary verifies the declared MP3 frame count before loudness comparison.
