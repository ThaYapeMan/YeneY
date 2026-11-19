# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Reachability cases against real LMS and an independently clocked renderer."""
from contextlib import closing
import json
import os
import time

from queue_sonos_fake import request
from timing_room_real_lms import app_position, query_clock


def run(c, case):
    cli, mac, wait, sonos, items, base = (c[k] for k in ('cli', 'mac', 'wait', 'sonos', 'items', 'base'))
    sonos.realtime = True
    log = lambda: (base / 'bridge.log').read_text()
    status_path = base / ('stream-pacing-' + b'Study'.hex() + '.json')
    status = lambda: json.loads(status_path.read_text())['playback']
    source = os.environ['YENEY_POSITION_SOURCE']
    precise = source == 'websocket'
    tolerance = 30 if precise else 1250  # Same as the navigation position checks.
    load = 'playlistcontrol cmd:load track_id:' + ','.join(c['window_ids'][:2])
    index = lambda: int(cli('playlist index ?', mac)[-1])
    mode = lambda: cli('mode ?', mac)[-1]
    marks = [len(log()), len(sonos.requests), len(c['proxy'].records)]
    elapsed = None

    def journal():
        return log()[marks[0]:]

    def writes():
        return [action for action, _, _, _ in sonos.requests[marks[1]:]
                if not action.startswith('Get') and action != 'Browse']

    def mutations():
        return [row['request'][len(mac)+1:] for row in c['proxy'].records[marks[2]:]
                if row['request'].startswith(mac + ' ') and
                row['request'][len(mac)+1:].split(' ')[0] in ('time', 'playlist', 'pause', 'stop', 'play')
                and not row['request'].endswith(' ?')]

    def start():
        cli(load, mac)
        wait(lambda: status()['state'] == 'PLAYING' and len(items()) == 2 and
             status()['position_source'] == source, 15, 'playing both owned rows')

    def settled():
        nonlocal elapsed
        _, value, truth, held = query_clock(c)
        elapsed = value - truth
        return not held and abs(elapsed) <= tolerance

    def cache_audio():
        connection, response = request(items()[0]['uri'])
        with closing(connection), closing(response):
            audio = response.read()
        assert audio.startswith(b'RIFF') and len(audio) == int.from_bytes(audio[4:8], 'little') + 8

    def wake():
        wait(lambda: 'queue speaker answering' in journal() or 'queue end decision=fault' in journal(),
             max(0, sonos.silent_until - time.monotonic()) + 5, 'silence has ended')
        evidence = [line for line in journal().splitlines() if
                    'queue end decision=' in line or 'queue speaker silent' in line or
                    'queue speaker answering' in line]
        print('REACH evidence ' + json.dumps(evidence), flush=True)
        assert 'queue end decision=fault' not in journal(), evidence
        assert journal().count('queue speaker silent ') == 1, evidence
        assert journal().count('queue speaker answering ') == 1, evidence
        wait(lambda: status()['speaker_silent_ms'] == 0, 3, 'answered attempt visible')

    if case == 'startup':
        sonos.silence(5)
        start()
        wake()
        assert writes().count('Play') == 1
        assert not mutations()
    else:
        start()
        wait(settled, 4, 'clock initially settled')
        if case in ('app-scrub', 'lms-scrub', 'boundary'):
            cache_audio()
        if case == 'paused':
            cli('pause 1', mac)
            wait(lambda: status()['state'] == 'PAUSED', 4, 'paused before silence')
        elif case == 'app-scrub':
            seek_count = len(sonos.actions('Seek'))
            cli('time 12', mac)
            wait(lambda: len(sonos.actions('Seek')) > seek_count and
                 sonos.position.milliseconds() >= 12000 and settled(),
                 8, 'start offset above app rewind')
            # Keep the observed app movement outside the earlier LMS seek's
            # echo interval, while staying inside the same audible track.
            time.sleep(max(0, sonos.actions('Seek')[-1][3] + 3 - time.monotonic()))
        elif case == 'boundary':
            app_position(sonos, 21)
            wait(lambda: float(cli('time ?', mac)[-1]) >= 20 and settled(), 3, 'near the track end')
        marks[:] = [len(log()), len(sonos.requests), len(c['proxy'].records)]

    if case.startswith('steady-'):
        duration = int(case.split('-')[1])
        _, before, _, _ = query_clock(c)
        start_at = sonos.silence(duration)
        time.sleep(max(0, start_at + duration - .1 - time.monotonic()))
        _, during, _, _ = query_clock(c)
        assert during - before >= duration * 500, (before, during, duration)
        wake()
        assert not writes() and not mutations(), (writes(), mutations())
    elif case == 'journal':
        wait(lambda: bool(getattr(sonos, 'callback', '')), 3, 'AVTransport subscription available')
        connection, response = request(items()[0]['uri'])
        with closing(connection), closing(response):
            assert response.read(16384).startswith(b'RIFF')
        closed = time.monotonic()
        start_at = sonos.silence(2, after=1.5, action='GetPositionInfo', once=True)
        time.sleep(max(0, start_at - time.monotonic()))
        sonos.notify_transport({'TransportStatus': 'OK'})
        wake()
        assert len(sonos.silence_misses) == 1
        closed_to_unanswered = sonos.silence_misses[0][1] - closed
        assert 1.5 <= closed_to_unanswered <= 1.7, closed_to_unanswered
        assert 'end=speaker-closed' in journal()
        print(f'REACH closed_to_unanswered_s={closed_to_unanswered:.6f}', flush=True)
        assert not writes() and not mutations(), (writes(), mutations())
    elif case == 'lms-next':
        cli('playlist index 1', mac)
        wait(lambda: 'Seek' in writes(), 5, 'LMS Next issued Seek')
        seek = sonos.actions('Seek')[-1][3]
        sonos.silence(2, after=max(0, seek + .1 - time.monotonic()))
        wake()
        wait(lambda: index() == 1 and settled(), 5, 'LMS Next finished')
        assert writes().count('Seek') == 1 and not mutations(), (writes(), mutations())
    elif case == 'app-next':
        sonos.silence(2)
        wait(lambda: sonos.silence_misses, 2, 'silent poll before app Next')
        c['select_item'](1)
        wake()
        wait(lambda: index() == 1, 8, 'app Next reached LMS')
        assert not writes() and mutations() == ['playlist index 1'], (writes(), mutations())
    elif case in ('lms-scrub', 'app-scrub'):
        sonos.silence(2)
        wait(lambda: sonos.silence_misses, 2, 'silent poll before scrub')
        if case == 'lms-scrub':
            cli('time 12', mac)
        else:
            scrub_at = time.monotonic()
            app_position(sonos, 4)
        wake()
        if case == 'app-scrub':
            wait(lambda: any(cmd.startswith('time ') for cmd in mutations()), 8, 'app rewind reached LMS')
        wait(lambda: settled() and float(cli('time ?', mac)[-1]) >= (12 if case == 'lms-scrub' else 4),
             8, 'same scrub position after recovery')
        if case == 'lms-scrub':
            assert writes().count('Seek') == 1 and not mutations(), (writes(), mutations())
        else:
            assert not writes() and len(mutations()) == 1, (writes(), mutations())
            requested = float(mutations()[0].split()[1])
            sent = next(row['when'] for row in c['proxy'].records[marks[2]:] if
                        row['request'] == mac + ' ' + mutations()[0])
            assert 4 <= requested <= 4 + sent - scrub_at, (requested, sent - scrub_at)
    elif case == 'boundary':
        sonos.silence(5)
        wait(lambda: sonos.position.milliseconds() >= 24000, 5, 'natural fixture boundary')
        c['select_item'](1)
        wake()
        wait(lambda: index() == 1, 8, 'LMS reached natural successor')
        assert not writes() and not mutations(), (writes(), mutations())
    elif case == 'paused':
        sonos.silence(15)
        wake()
        assert status()['state'] == 'PAUSED' and mode() == 'pause'
        assert not writes() and not mutations(), (writes(), mutations())
        cli('pause 0', mac)
        wait(lambda: status()['state'] == 'PLAYING', 5, 'resume after silence')
    elif case in ('gone', 'back'):
        start_at = sonos.silence(25, 'all')
        wait(lambda: 'reason=speaker-unreachable' in journal(), 22, 'one unreachable decision')
        fault_at = time.monotonic() - start_at
        wait(lambda: mode() == 'stop', 3, 'unreachable stopped LMS')
        time.sleep(max(0, start_at + 26 - time.monotonic()))
        print(f'REACH fault_s={fault_at:.6f} writes={writes()} mutations={mutations()}', flush=True)
        assert abs(fault_at - 20) <= 1, (fault_at, journal())
        assert journal().count('queue end decision=fault ') == 1
        assert journal().count('queue speaker silent ') == 1 and 'queue speaker answering ' not in journal()
        assert status()['state'] == 'STOPPED' and mutations() == ['stop'] and not writes()
        assert sonos.verified_save and sonos.original in sonos.snapshots.values()
        if case == 'back':
            start()
            assert writes().count('Play') == 1, writes()
            print('REACH pass new LMS play after unreachable', flush=True)
        return
    elif case == 'answered-fault':
        sonos.read_fault_once = ('GetPositionInfo', '701')
        wait(lambda: 'queue end decision=fault' in journal(), 5, 'answered read fault ends room')
        print('REACH evidence ' + next(line for line in journal().splitlines() if 'queue end decision=fault' in line),
              flush=True)
        assert 'reason=queue-soap-error action=GetPositionInfo fault=701' in journal()
        assert 'queue speaker silent' not in journal()
        wait(lambda: mutations() == ['stop'], 5, 'answered fault stopped LMS once')
        return
    else:
        assert case == 'startup', case

    wait(settled, 5, 'clock settled after silence')
    assert status()['state'] == 'PLAYING'
    print(f'REACH pass case={case} source={source} error_ms={elapsed:.6f} writes={writes()} mutations={mutations()}',
          flush=True)
