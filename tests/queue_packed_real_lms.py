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
                              speed=8 if mode == 'benchmark' else 1)
    sonos.device_audio = model
    paths = [base / f'input-{i}.wav' for i in range(3)]
    expected = [(path.read_bytes() if mode == 'benchmark' else expected_wav(path))[44:] for path in paths]
    limit = 60 if mode == 'benchmark' else 45
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
        if mode not in ('controls', 'fixed-controls'):
            for index in (1, 2):
                wait(lambda: position() == rows[index]['uri'], limit, f'natural join {index}')
                wait(lambda: int(cli('playlist index ?', mac)[-1]) == index, 8, f'LMS follows join {index}')
            wait(lambda: sonos.transport_state == 'STOPPED', limit, 'all three rows consumed')
            assert model.audio == b''.join(expected), (len(model.audio), [len(part) for part in expected])
            assert not sonos.errors, sonos.errors
            log = decisions()
            print('\n'.join(log), flush=True)
            if mode == 'benchmark':
                with open(f'/proc/{bridge.pid}/status') as status:
                    print(next(line.strip() for line in status if line.startswith('VmHWM:')), flush=True)
                print(f'PASS five-minute-track album, elapsed {time.monotonic()-start:.2f} s', flush=True)
                return
            forms = [re.search(r'pack=(\w+) reason=(\w[\w-]*)', line).groups() for line in log]
            assert len(forms) == 3 and forms[1:] == [('packed', 'ready')] * 2, log
            initial_form, initial_reason = forms[0]
            # The eight-second first row may finish packing before the device asks.
            assert (initial_form == 'packed') == (initial_reason == 'ready'), log
            print(f'PASS real LMS initial form={initial_form}, two packed successors, '
                  'sample-exact joins and LMS indices', flush=True)
            return
        if mode == 'fixed-controls':
            wait(lambda: model.current is not None and model.frame >= 44100,
                 7, 'fixed cold start at frame zero')
            assert not model.row.header.packed_length, decisions()
        else:
            wait(lambda: model.current is not None and model.current['uri'] == items()[0]['uri'] and
                 model.frame >= 6 * 44100, 10, 'prefetch lead before Next')
            cli('playlist index 1', mac)
            wait(lambda: int(cli('playlist index ?', mac)[-1]) == 1 and position() == items()[1]['uri'] and
                 model.current is not None and model.current['uri'] == position() and model.frame > 0,
                 10, 'LMS Next')
            assert model.row.header.packed_length, decisions()
        # Exercise the seek and resume request shape on the selected row.
        before = len(model.requests)
        model.seek(3)
        wait(lambda: sonos.transport_state == 'PLAYING', 3, 'seek rebuffering ends')
        time.sleep(1)
        model.pause()
        sonos.notify_transport()
        wait(lambda: cli('mode ?', mac)[-1] == 'pause', 4, 'LMS follows pause')
        time.sleep(5)
        sonos.flac_restart_capture = 'port'
        model.resume()
        sonos.notify_transport()
        wait(lambda: cli('mode ?', mac)[-1] == 'play', 4, 'LMS follows resume')
        sequences = [(method, offset, code) for method, _, offset, code in model.requests[before:]]
        assert any(offset and offset % 16384 == 0 for _, offset, _ in sequences)
        assert sum(code == 'probe-closed' for _, _, code in sequences) >= 2
        print('Captured restart request shapes:', sequences, flush=True)
        assert not sonos.errors, sonos.errors
        print(f'PASS real LMS {mode} capture-based seek and pause requests', flush=True)
    finally:
        model.close()
        del sonos.device_audio
