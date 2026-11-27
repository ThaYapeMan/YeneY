# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Keep one real LMS room playing while another produces a long queue row."""
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

from queue_sonos_fake import FiniteDeviceAudio

ROOT = Path(__file__).resolve().parents[1]


def rss_kib(process):
    text = Path(f'/proc/{process.pid}/status').read_text()
    return int(next(line.split()[1] for line in text.splitlines() if line.startswith('VmHWM:')))


def cancel_productions(context, model, log):
    cli, mac, ids, sonos, wait = (
        context[key] for key in ('cli', 'mac', 'window_ids', 'sonos', 'wait'))
    for index, command in ((1, 'playlist index 1'), (3, 'time 50'), (5, 'stop')):
        cli('stop', mac)
        wait(lambda: sonos.transport_state == 'STOPPED', 8, 'previous run stopped')
        offset = len(log.read_text())
        previous_uri = sonos.position['TrackURI']
        requests = len(model.requests)
        cli('playlistcontrol cmd:load track_id:' + ','.join(ids[index:index + 2]), mac)
        wait(lambda: sonos.transport_state == 'PLAYING' and
             sonos.position['TrackURI'] != previous_uri and len(model.requests) > requests,
             10, 'reader opened during production')
        uri = sonos.position['TrackURI']
        before = log.read_text()[offset:]
        occurrences = re.findall(r'queue decode occurrence=(\d+) first audio', before)
        assert occurrences, before
        active = occurrences[0]
        assert not re.search(rf'queue row complete occurrence={active} ', before), before

        cli(command, mac)
        if command == 'stop':
            wait(lambda: sonos.transport_state == 'STOPPED', 8, 'stop cancels producer')
        elif command.startswith('time'):
            wait(lambda: float(cli('time ?', mac)[-1]) >= 49 and len(model.requests) > requests + 1,
                 10, 'seek opens a reader at the new position')
        else:
            wait(lambda: cli('playlist index ?', mac)[-1] == '1' and
                 sonos.position['TrackURI'] != uri and model.current and
                 model.current['uri'] == sonos.position['TrackURI'] and model.frame > 0,
                 10, 'next row audible')
        assert not sonos.errors, sonos.errors
        after = log.read_text()[offset:]
        assert 'queue worker fault=' not in after and 'core: fatal:' not in after, after
        print(f'PASS cancelled active production occurrence={active} command={command}', flush=True)


def room(context, role):
    cli, mac, base, sonos, wait, tap, bridge = (
        context[key] for key in ('cli', 'mac', 'base', 'sonos', 'wait', 'tap', 'bridge'))
    folder = Path(os.environ['YENEY_WORK_DRIVEN_SYNC'])
    log = base / 'bridge.log'
    model = FiniteDeviceAudio(sonos, progressive=True, prefetch=False)
    sonos.device_audio = model
    model.start()
    try:
        if role == 'producing':
            wait(lambda: (folder / 'playing').exists(), 30, 'other room playing')
        cli('playlistcontrol cmd:load track_id:' + context['window_ids'][0], mac)
        if role == 'playing':
            wait(lambda: model.frame >= 44100, 12, 'first room in steady play')
            origin = model.frame
            (folder / 'playing').touch()
            wait(lambda: (folder / 'produced').exists(), 30, 'other room row completed')
            assert model.frame > origin and sonos.transport_state == 'PLAYING'
            assert not model.real_eof and not sonos.errors, sonos.errors
            assert not any(p.get('event') in ('STMo', 'STMn') for p in tap.records), tap.records
            stat = [p for p in tap.records if p.get('event') == 'STMt']
            assert stat, tap.records
            elapsed = struct.unpack('!4sBBBIIIIHIIIIHIIH', bytes.fromhex(stat[-1]['body']))[14]
            observed = model.clock_frame * 1000 / 44100
            assert abs(observed - elapsed) <= 2000, (observed, elapsed)
            print(f'PASS steady room while another produces: frames_advanced={model.frame-origin} '
                  f'speaker_ms={observed:.0f} LMS_ms={elapsed} peak_kib={rss_kib(bridge)}', flush=True)
        else:
            marker = 'yeney: queue row complete occurrence=1 frames=7938000 '
            wait(lambda: marker in log.read_text(), 25, 'all 7,938,000 frames produced')
            completion = next(line for line in log.read_text().splitlines() if marker in line)
            print(f'PASS long row produced while another room played: {completion} '
                  f'peak_kib={rss_kib(bridge)}', flush=True)
            (folder / 'produced').touch()
            cancel_productions(context, model, log)
        cli('stop', mac)
    finally:
        model.close()
        del sonos.device_audio


def check():
    with tempfile.TemporaryDirectory(prefix='yeney-work-driven-') as temporary:
        base = Path(temporary)
        children = []
        try:
            for role in ('playing', 'producing'):
                target = base / role
                target.mkdir()
                env = dict(os.environ, YENEY_WORK_DRIVEN_ROOM=role,
                           YENEY_WORK_DRIVEN_SYNC=str(base), YENEY_TEST_CACHE=str(target),
                           YENEY_QUEUE_DISK_MIB='256', YENEY_FULLQ_QUOTA_MIB='256')
                with (target / 'run.log').open('w') as log:
                    child = subprocess.Popen(
                        ['bwrap', '--unshare-net', '--die-with-parent', '--bind', '/', '/',
                         '--dev', '/dev', '--tmpfs', '/dev/shm',
                         sys.executable, str(ROOT / 'tests' / 'real_lms_test.py')],
                        env=env, stdout=log, stderr=subprocess.STDOUT,
                    )
                children.append((child, target / 'run.log'))
            for child, log in children:
                status = child.wait(timeout=100)
                print(log.read_text(), end='', flush=True)
                assert status == 0, (log, status)
        finally:
            for child, _ in children:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=15)


if __name__ == '__main__':
    check()
