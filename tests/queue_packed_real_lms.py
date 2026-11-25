# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Use the real LMS with the captured fixed and packed restart request shapes."""
import os
import re
import time

from queue_pacing_study_test import expected_wav
from queue_sonos_fake import FiniteDeviceAudio


def run(context):
    cli, mac, base, sonos, items, wait, bridge = (
        context[key] for key in ('cli', 'mac', 'base', 'sonos', 'items', 'wait', 'bridge'))
    mode = os.environ['YENEY_QUEUE_PACKED_REAL_LMS']
    sonos.flac_restart_capture = 'study'
    model = FiniteDeviceAudio(sonos, progressive=True, prefetch=False,
                              speed=20 if mode == 'benchmark' else 1)
    sonos.device_audio = model
    expected = [expected_wav(base / f'input-{i}.wav')[44:] for i in range(3)]
    position = lambda: sonos.position['TrackURI']
    decisions = lambda: [line for line in (base / 'bridge.log').read_text().splitlines()
                         if 'queue row form occurrence=' in line]
    start = time.monotonic()
    try:
        model.start()
        album = 'playlistcontrol cmd:load track_id:' + ','.join(context['window_ids'][:3])
        cli(album, mac)
        wait(lambda: len(items()) == 3 and sonos.transport_state == 'PLAYING', 15, 'three local rows')
        rows = items()
        for index in (1, 2):
            wait(lambda: position() == rows[index]['uri'], 45, f'natural join {index}')
            wait(lambda: int(cli('playlist index ?', mac)[-1]) == index, 8, f'LMS follows join {index}')
        wait(lambda: sonos.transport_state == 'STOPPED', 45, 'all three rows consumed')
        assert model.audio == b''.join(expected), (len(model.audio), [len(part) for part in expected])
        assert not sonos.errors, sonos.errors
        log = decisions()
        print('\n'.join(log), flush=True)
        if mode == 'benchmark':
            with open(f'/proc/{bridge.pid}/status') as status:
                print(next(line.strip() for line in status if line.startswith('VmHWM:')), flush=True)
            print(f'PASS five-minute-track album, elapsed {time.monotonic()-start:.2f} s', flush=True)
            return
        assert [re.search(r'pack=(\w+)', line)[1] for line in log] == ['fixed', 'packed', 'packed'], log
        print('PASS real LMS cold fixed row and two packed successors, sample-exact joins and LMS indices', flush=True)
        cli('stop', mac)
        wait(lambda: not items(), 10, 'album cleanup')
        sonos.flac_restart_capture = 'port'
        model.audio.clear()
        cli(album, mac)
        wait(lambda: len(items()) == 3 and model.current is not None and model.frame >= 2 * 44100,
             15, 'second cold start')
        wait(lambda: model.current is not None and model.current['uri'] == items()[0]['uri'] and
             model.frame >= 6 * 44100, 10, 'prefetch lead before Next')
        cli('playlist index 1', mac)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == 1 and position() == items()[1]['uri'] and
             model.current is not None and model.current['uri'] == position() and model.frame > 0,
             10, 'LMS Next')
        assert model.row.header.packed_length, decisions()
        # Exercise the capture shape on the packed row now selected by LMS.
        before = len(model.requests)
        model.seek(3)
        model.pause()
        time.sleep(5)
        model.resume()
        assert any(offset and offset % 16384 == 0 for _, _, offset, _ in model.requests[before:])
        assert not sonos.errors, sonos.errors
        print('PASS real LMS Next and capture-based seek and pause requests', flush=True)
    finally:
        model.close()
        del sonos.device_audio
