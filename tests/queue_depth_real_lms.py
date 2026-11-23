# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Source depth and sample identity from the native decoder to the renderer."""

from contextlib import closing

from queue_pacing_study_test import expected_wav
from queue_sonos_fake import FiniteDeviceAudio, request
from served_queue_audio import RowReader


def run(context):
    cli, mac, base, sonos, items, wait = (
        context[key] for key in ("cli", "mac", "base", "sonos", "items", "wait"))
    ids = context["window_ids"]

    def load(indices):
        cli("playlistcontrol cmd:load track_id:" + ",".join(ids[i] for i in indices), mac)
        wait(lambda: len(items()) == len(indices) and sonos.transport_state == "PLAYING",
             15, "depth fixture queue")
        return items()

    def fetch(row, bits):
        assert row["resource"]["bitsPerSample"] == str(bits), row["resource"]
        client, response = request(row["uri"])
        with closing(client), closing(response):
            assert response.status == 200 and response.getheader("Content-Type") == "audio/flac"
            reader = RowReader(response)
            assert reader.header.bits == bits, reader.header
            if "size" in row["resource"]:
                assert int(row["resource"]["size"]) == reader.header.wire_length
            return reader.read_pcm()

    first, second = load((0, 1))
    first_pcm = expected_wav(base / "input-0.wav")[44:]
    second_pcm = (base / "input-1.wav").read_bytes()[44:]
    assert fetch(first, 16) == first_pcm
    print("PASS native 16-bit row restores the padded 24-bit PCM exactly", flush=True)

    model = FiniteDeviceAudio(sonos, progressive=True, prefetch=False)
    sonos.device_audio = model
    model.start()
    try:
        wait(lambda: sonos.position["TrackURI"] == second["uri"], 15, "16-to-24-bit join")
        wait(lambda: sonos.transport_state == "STOPPED", 15, "24-bit row end")
        assert bytes(model.audio) == first_pcm + second_pcm
        assert not sonos.errors, sonos.errors
        print("PASS natural join between 16-bit and 24-bit FLAC rows", flush=True)
    finally:
        model.close()
        del sonos.device_audio

    cli("stop", mac)
    wait(lambda: not items(), 10, "release mixed-depth queue")
    row, = load((1,))
    assert fetch(row, 24) == second_pcm
    print("PASS native 24-bit source retains every low byte", flush=True)

    cli("stop", mac)
    wait(lambda: not items(), 10, "release 24-bit queue")
    cli("playerpref replayGainMode 1", mac)
    row, = load((2,))
    pcm = fetch(row, 24)
    assert len(pcm) == len(expected_wav(base / "input-2.wav")) - 44
    assert pcm != expected_wav(base / "input-2.wav")[44:]
    assert any(pcm[::3])
    print("PASS processed native 16-bit source uses 24-bit FLAC", flush=True)
