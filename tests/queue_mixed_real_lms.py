# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Play two finite loopback HTTP tracks with a local track between them."""

from queue_pacing_study_test import expected_wav
from queue_sonos_fake import FiniteDeviceAudio


def run(context):
    cli, mac, base, sonos, items, wait = (
        context[key] for key in ("cli", "mac", "base", "sonos", "items", "wait"))
    before, local, after = 0, 2, 1
    sources = [context["Remote"]((base / f"library/{index}.flac").read_bytes())
               for index in (before, after)]
    context["extra_remotes"].extend(sources)
    # LMS learns the duration of a new HTTP URL only after opening it.
    for source in sources:
        cli("playlist play " + source.url, mac)
        wait(lambda: items() and sonos.transport_state == "PLAYING" and
             "duration:24" in cli("status 0 1 tags:d", mac), 10, "LMS learned remote duration")
        cli("stop", mac)
        wait(lambda: not items(), 10, "release duration probe")
    for command in ("playlist clear", "playlist add " + sources[0].url,
                    "playlistcontrol cmd:add track_id:" + context["window_ids"][local],
                    "playlist add " + sources[1].url, "playlist index 0"):
        cli(command, mac)
    wait(lambda: len(items()) == 3 and sonos.transport_state == "PLAYING", 15, "mixed queue starts")
    rows = items()
    forms = [row["resource"]["protocolInfo"].split(":")[2] for row in rows]
    assert forms == ["audio/wav", "audio/flac", "audio/wav"], rows
    assert all(row["uri"].endswith("." + form.split("/")[1]) for row, form in zip(rows, forms))
    expected_pcm = [expected_wav(base / f"input-{index}.wav")[44:] for index in (before, local, after)]
    model = FiniteDeviceAudio(sonos, progressive=True, prefetch=False)
    sonos.device_audio = model
    model.start()
    try:
        for index in (1, 2):
            wait(lambda: sonos.position["TrackURI"] == rows[index]["uri"], 35,
                 f"natural mixed-form join into row {index + 1}")
            wait(lambda: int(cli("playlist index ?", mac)[-1]) == index, 8,
                 f"LMS follows mixed-form join {index}")
        wait(lambda: sonos.transport_state == "STOPPED", 35, "mixed queue fully audible")
        assert model.audio == b"".join(expected_pcm), (len(model.audio), [len(part) for part in expected_pcm])
        assert not sonos.errors, sonos.errors
        print("PASS real LMS natural WAV to FLAC to WAV joins at default pacing, sample-exact rows", flush=True)
    finally:
        model.close()
        del sonos.device_audio
