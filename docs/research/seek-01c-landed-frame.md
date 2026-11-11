# SEEK-01c: core ALAC landed-frame stop condition

## Decision

Stop this round under the task's explicit instruction: "Bridge-only unless the
investigation proves the landed frame for an M4A seek is wrong in the core; in
that case stop, record the finding under Decisions with evidence, and do not
change the core or its pin (`9fba26f`) in this round."

The native ALAC decoder reports frame zero for an LMS seek response whose first
4096 stereo PCM frames exactly equal original source frames 1822720–1826815.
The core landing metadata is therefore incorrect for this response. No bridge
fix, core change or pin change is included. This is an investigation record,
not a release declaring the worker exception defect fixed.

## Byte-level evidence

Date: 2026-11-11. Bridge baseline: `23aa46caa6dc1fc411214c5daf80f24ee16c5996`.
Core and gitlink: `9fba26f0bcc9a10def8063c5d269b2bbe62b687a`.
Server: isolated test instance of LMS 9.1.1, loopback endpoints only.
File: `/home/codex/private-fixtures/04 I Put A Spell On You.m4a`, read in place
through a library symlink. Its audio is ALAC, stereo, 24-bit, 96000 Hz.
No encoded media or decoded PCM was written to diagnostic artifacts.

The private files have no ReplayGain tags. Set the temporary LMS library row's
`replay_gain=-4.4` and `replay_peak=1`, and enable player `replayGainMode=1`.
This changes only the disposable server database, never the private file.
Both starts log `core: track gain=39424 transition=0 period=10`.
The server's quantized gain, rather than an assumed exact floating conversion,
is the value used on both productions.

Load the file, read the completed natural WAV, then issue LMS `time 19`.
An HTTP proxy observes the response body in memory before forwarding it to the
bridge. Decode the first 8 MiB of that response using the unchanged core's
`makeDecoder`, `codec='l'`, `maxRate=192000`, `finitePcmContainers=true`, with
conversion disabled, and take its first 4096 frames. Query `sourceStart`.
Compare those normalized signed 32-bit stereo frames, in memory, with FFmpeg's
lossless decode of the original ALAC. This examines source coordinates before
ReplayGain and decimation; neither can explain a wrong source-start value.

```text
RAW request=1 decoder_exit=0 signature=0000001c stderr=
RAW reported_frame=0 true_frame=-1 frames=4096 sha256=f2b740dc281e0076a885214ac11e5dc4169c993f35e4eb32027212ab61cca598
REFERENCE bytes=61071404
RAW request=2 decoder_exit=0 signature=0000001c stderr=
RAW reported_frame=0 true_frame=1822720 frames=4096 sha256=90fea58a1c8eec11f3961a2bf6dc33a0c1ab81b7631d2ad5a679af179aa08feb
```

The first response's `true_frame=-1` means only that it did not occur in the
18–21 second search interval. A separate comparison establishes that its hash
is exactly the original file's first 4096 frames. The seek response hash equals
original frames starting at 1822720 (445 × 4096), or 18.9866666667 seconds.
The first different frame when comparing the seek response with its reported
origin is frame 0; the first differing byte is byte 1 of normalized s32le PCM.
That coordinate is not a claim about a retained 24-bit WAV difference offset.

The original-file check produced:

```json
{
  "origin_sha256": "f2b740dc281e0076a885214ac11e5dc4169c993f35e4eb32027212ab61cca598",
  "seek_sha256": "90fea58a1c8eec11f3961a2bf6dc33a0c1ab81b7631d2ad5a679af179aa08feb",
  "first_differing_frame": 0,
  "first_differing_byte_in_s32_frames": 1,
  "reported_frame": 0,
  "true_first_frame": 1822720,
  "true_first_seconds": 18.986666666666668,
  "original_file_pcm_compared_in_memory_only": true
}
```

The MP4 header is larger than 512 KiB: the first audio packet is at original
file offset 4045756. Initial probes of only 512 KiB emitted no audio and were
inconclusive. Increasing the prefix to 8 MiB produced the successful comparison.
Only the complete comparison above supports the decision.

## Mechanism and limits

`third_party/yeney-core/core/decoder.cpp:622` sets ALAC source start from
`info.trimBegin`. That trim coordinate is zero in the rewritten seek stream;
it does not recover its position in the original file. MP4 packet/chunk tables
are relative to the rewritten response. A syntactically valid zero source start
is consequently not proof that this producer begins at original frame zero.
This is category (a): a wrong source landing coordinate, confirmed before gain.
It is not evidence of nondeterministic gain or a bridge comparison error.

In the actual bridge run, the 96000→48000 conversion masks that source position
as unknown (`core/decimating_decoder.cpp:212`). The renderer therefore takes the
existing `reason=landed-frame-unknown` fallback. Its diagnostic conversion label
says `queue FLAC` even for this observed native MP4 body; this label alone does
not identify the wire codec. The response signature and successful native ALAC
decode establish that the inspected body was MP4/ALAC.

This run does not reproduce the owner's 44100-Hz process abort and does not
establish its entire causal chain. It does establish the task's explicit core
M4A-landing stop condition. Do not present the private S1 contract as passing:
it fails because it requires in-item attachment and this run falls back.
Do not infer a safe bridge alignment from requested seconds; packet landing
is 1280 source frames before the requested 1824000.

## Source-type observations

| Source path | Observed result | Reason / limitation |
| --- | --- | --- |
| Private 24/96 ALAC through core 96→48 conversion | Fallback | `landed-frame-unknown`; conversion deliberately invalidates sourceStart |
| Native ALAC decoder before conversion on the same seek response | Incorrect known landing | Reports 0; bytes prove original frame 1822720; core investigation required |
| Generated WAV/FLAC paths | No new acceptance claim | Existing SEEK-01b implementation retained; not rerun in the stopped round |
| Other source types, changed gains, injected corruption | Not established | R1/R3 and C1–C5 remain outstanding |

## Outstanding work

R1 (nonfatal worker decisions), I6 enforcement/documentation, R3 media gates,
C1–C5 before/after and the three soak seeds are not completed. No full `make
test` or consumer `require_v1` success is claimed. Resolve the core landing
contract in a separately authorized round, then resume the bridge work and its
full acceptance requirements. The task explicitly forbids changing that core
or its pin in this round. No deployment or owner endpoint access took place.

## Closing follow-up — CORE-ALAC-SEEK-01 and SEEK-01d

Core `a9ab672f147fe23650ec2a7b18ef1a96bd61fe36` fixes the ALAC landing
contract: native unconverted output reports the original source coordinate;
unprovable or converted output reports unknown. Known zero is never a default.
Its from-start trimmed production begins at source frame 113, while an interior
seek retains its original coordinate. SEEK-01d pins this core and subtracts the
retained WAV origin at attach. It also converts overlap mismatch into a logged
fallback and gates publication on exact verification. Generated ALAC/ReplayGain
cases now cover this path independently of the owner's private media. The
SEEK-01d report records the actual acceptance status of each gate.
