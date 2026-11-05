# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Navigation invariants against private LMS and an echoing queue renderer."""
import concurrent.futures
import hashlib
import http.client
import json
import os
import random
import re
import time
from pathlib import Path
from urllib.parse import urlsplit


def run(c):
    cli, mac, wait, items, select, sonos, proxy, base, ids = (
        c[k] for k in ('cli', 'mac', 'wait', 'items', 'select_item', 'sonos',
                       'proxy', 'base', 'window_ids'))
    case = os.environ.get('YENEY_NAV_CASE', 'F1')
    binary = os.environ.get('YENEY_ACCEPTANCE_BINARY', str(c['ROOT'] / 'yeney'))
    print('NAV binary sha256=' + hashlib.sha256(Path(binary).read_bytes()).hexdigest(), flush=True)
    log = lambda: (base / 'bridge.log').read_text()
    def bridge_at(index):
        path = base / ('stream-pacing-' + b'Study'.hex() + '.json')
        if not path.exists(): return False
        status = json.loads(path.read_text()).get('playback', {})
        return status.get('title') == f'Title & {index}' and status.get('state') == 'PLAYING'
    def fetch(item, prefix=False):
        url = urlsplit(item['uri'])
        conn = http.client.HTTPConnection(url.netloc, timeout=8)
        try:
            conn.request('GET', url.path)
            r = conn.getresponse()
            body = r.read(44 + 4096) if prefix else r.read()
            assert r.status == 200 and len(body) > 44 and body[:4] == b'RIFF'
            return body
        finally:
            conn.close()
    def row(index):
        return next(i for i in items() if i['metadata']['title'] == f'Title & {index}')
    def audible(index):
        select(items().index(row(index)))
    cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
    wait(lambda: len(items()) == 4, 15, 'eight-item initial window')
    select(0)
    mark = len(log())
    if case == 'F1':
        before = len(proxy.records)
        # Queue a second live request while the first selection is settling.
        targets = [row(1), row(2), row(3)]
        with concurrent.futures.ThreadPoolExecutor(3) as pool:
            futures = []
            for target in targets:
                futures.append(pool.submit(fetch, target, True))
                time.sleep(.08)
            for f in futures:
                f.result()
        audible(3)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == 3, 8, 'rapid Next final index')
        assert 'lazy abandoned' not in log()[mark:], log()[mark:]
        print('PASS F1 rapid Next x3: every live GET served, no abandoned reader', flush=True)
        commands = [r['request'] for r in proxy.records[before:]
                    if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert commands == [mac + f' playlist index {n}' for n in (1, 2, 3)], commands
        wait(lambda: bridge_at(3), 5, 'rapid burst audible confirmation')
        # Earlier published selections must retire when the last burst row is
        # confirmed, so Back is a new selection rather than an old binding.
        before_back = len(proxy.records)
        audible(1)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == 1 and bridge_at(1),
             8, 'Back after burst retires earlier published selections')
        fetch(row(1), True)
        back_commands = [r['request'] for r in proxy.records[before_back:]
                         if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert back_commands == [mac + ' playlist index 1'], back_commands
        print('PASS F1 Back after burst: older published selections retired, one fresh command', flush=True)

        for scenario in ('different-index', 'same-index', 'next-back', 'next-x2'):
            cli('stop', mac)
            wait(lambda: not items(), 10, 'targeted release')
            cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
            wait(lambda: len(items()) == 4, 15, 'targeted fresh window')
            select(0)
            reference = fetch(row(0))
            before, local_mark = len(proxy.records), len(log())
            target = row(1)
            with concurrent.futures.ThreadPoolExecutor(2) as pool:
                audible(1)
                wait(lambda: any(' playlist index 1' == r['request'][len(mac):] for r in proxy.records[before:]),
                     5, 'speaker selection before GET')
                first = pool.submit(fetch, target, True)
                if scenario in ('different-index', 'next-x2'):
                    time.sleep(.08)
                    second = pool.submit(fetch, row(2), True)
                    first.result(); second.result()
                    audible(2)
                    final, sequence = 2, (1, 2)
                elif scenario == 'same-index':
                    second = pool.submit(fetch, target, True)
                    assert first.result() == second.result()
                    final, sequence = 1, (1,)
                else:
                    first.result()
                    audible(1)
                    wait(lambda: bridge_at(1), 5, 'Next audible before Back')
                    audible(0)
                    assert fetch(row(0)) == reference
                    final, sequence = 0, (1, 0)
            wait(lambda: int(cli('playlist index ?', mac)[-1]) == final, 8, scenario)
            time.sleep(.5)
            commands = [r['request'] for r in proxy.records[before:]
                        if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
            assert commands == [mac + f' playlist index {n}' for n in sequence], (scenario, commands)
            assert 'lazy abandoned' not in log()[local_mark:] and sonos.transport_state == 'PLAYING'
            print(f'PASS F1 {scenario}: served, commands={len(commands)}, final_index={final}', flush=True)

        cli('stop', mac)
        wait(lambda: not items(), 10, 'lazy Back release')
        cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
        wait(lambda: len(items()) == 4, 15, 'lazy Back window')
        select(0)
        cli('playlist index 5', mac)
        wait(lambda: bridge_at(5) and len(items()) == 3, 15, 'lazy Back starts at five')
        audible(7)
        fetch(row(7), True)
        wait(lambda: bridge_at(7), 5, 'lazy Back outgoing seven')
        assert any(i['metadata']['title'] == 'Title & 6' for i in items())
        local_mark, before = len(log()), len(proxy.records)
        audible(6)
        # Poll alone must preserve this unproduced history row until binding.
        wait(lambda: bridge_at(6), 8, 'unproduced Back target survives publication')
        fetch(row(6), True)
        assert 'lazy abandoned' not in log()[local_mark:] and sonos.transport_state == 'PLAYING'
        commands = [r['request'] for r in proxy.records[before:]
                    if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert commands == [mac + ' playlist index 6'], commands
        print('PASS F1 poll Back to unproduced history: publication waits for fresh audible anchor, one command', flush=True)
    elif case == 'F2':
        # Replace exactly one complete status response after the restart edit.
        with sonos.lock:
            sonos.position['RelTime'] = '00:00:08'
        wait(lambda: float(cli('time ?', mac)[-1]) >= 5, 5, 'restart clock')
        cli('button jump_rew', mac)
        wait(lambda: 'kind=previous' in log()[mark:], 15, 'index-zero restart')
        injected = []
        def transient(wire):
            if b' status 0 60000 ' in wire and not injected:
                injected.append(wire.decode().strip())
                return wire.rstrip() + b' mode%3Aplay playlist_tracks%3A0 playlist_cur_index%3A-1 playlist_timestamp%3Astale\n'
        proxy.request_hook = transient
        wait(lambda: injected, 5, 'transient empty snapshot read')
        time.sleep(.8)
        proxy.request_hook = None
        print('F2 injected snapshot: ' + injected[0] + ' -> playlist_tracks:0 mode:play', flush=True)
        assert len(items()) == 4 and sonos.transport_state == 'PLAYING', log()[mark:]
        assert 'rebuild reason=clear' not in log()[mark:], log()[mark:]
        assert 'snapshot rejected' in log()[mark:], log()[mark:]
        print('PASS F2 transient empty restart snapshot rejected, window intact, playback continues', flush=True)
        # A valid short response and empty response while a new navigation
        # producer is settling must not alter occurrence identities either.
        for target, short in ((1, False), (0, True), (0, False), (1, True)):
            injected = []
            def stale(wire):
                if b' status 0 60000 ' not in wire or injected:
                    return None
                injected.append(wire.decode().strip())
                fields = b' mode%3Aplay playlist_tracks%3A0 playlist_cur_index%3A-1 playlist_timestamp%3Astale'
                if short:
                    fields = (' mode:play playlist_tracks:1 playlist_cur_index:0 playlist_timestamp:short '
                              'playlist%20index:0 id:' + ids[0] + ' title:Title%20%26%200 duration:24 url:file%3A%2F%2Ffixture').encode()
                return wire.rstrip() + fields + b'\n'
            proxy.request_hook = stale
            cli('playlist index ' + str(target), mac)
            wait(lambda: injected, 5, 'navigation snapshot injection')
            proxy.request_hook = None
            wait(lambda: any(i['metadata']['title'] == f'Title & {target}' and
                 i['uri'] == sonos.position['TrackURI'] for i in items()), 10, 'navigation survives stale read')
            assert 'rebuild reason=clear' not in log()[mark:]
            assert sonos.transport_state == 'PLAYING'
            print(f'PASS F2 transient {"short" if short else "empty"} navigation index={target} rejected', flush=True)
        time.sleep(3.2)
        clear_mark = len(log())
        cli('playlist clear', mac)
        wait(lambda: not items() and sonos.transport_state == 'STOPPED', 10, 'genuine clear stops')
        assert 'rebuild reason=clear' in log()[clear_mark:], log()[clear_mark:]
        print('PASS F2 genuine confirmed clear rebuilds and stops', flush=True)

    elif case == 'F3':
        reference = fetch(row(0))
        time.sleep(.5)
        cli('time 12', mac)
        wait(lambda: 'kind=seek' in log()[mark:], 15, 'track-zero seek')
        partial = row(0)['uri']
        # Finish the partial so it really is retained history. Let natural
        # preparation declare its successor before the controller move, so
        # this setup cannot cancel a just-opening decoder connection.
        prepare_mark = len(log())
        fetch(row(0))
        wait(lambda: re.search(r'core: queue FLAC occurrence=\d+ exact remaining=\d+ first sample=0',
                               log()[prepare_mark:]), 8, 'natural successor declaration before F3 setup move')
        cli('playlist index 1', mac)
        wait(lambda: any(i['metadata']['title'] == 'Title & 1' and i['uri'] == sonos.position['TrackURI'] for i in items()), 15, 'navigate away from seek')
        before = len(proxy.records)
        mark = len(log())
        audible(0)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == 0, 8, 'speaker Back to partial')
        wait(lambda: any(i['metadata']['title'] == 'Title & 0' and i['uri'] != partial and i['uri'] == sonos.position['TrackURI'] for i in items()),
             8, 'fresh frame-zero target replaces seek partial')
        body = fetch(row(0))
        assert body == reference, 'fresh frame-zero WAV differs from natural production'
        commands = [r['request'] for r in proxy.records[before:]
                    if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert commands == [mac + ' playlist index 0'], commands
        assert sonos.transport_state == 'PLAYING', log()[mark:]
        assert 'window target re-produced lms_index=0 origin_ms=0 lms_commands=1' in log()[mark:]
        assert 'to_index=0 action=slide history=0 lms_commands=1 kind=previous' in log()[mark:]
        print('PASS F3 retained seek partial re-produced from frame zero, one command, no stop', flush=True)
    elif case == 'SOAK':
        seed = int(os.environ.get('YENEY_NAV_SEED', '1729'))
        rng = random.Random(seed)
        print(f'SOAK seed={seed} steps=200', flush=True)
        # Reference each track's natural, complete production, outside the soak.
        reference = {}
        for index in range(8):
            cli('stop', mac)
            wait(lambda: not items(), 10, 'reference release')
            cli('playlistcontrol cmd:load track_id:' + ids[index], mac)
            wait(lambda: len(items()) == 1, 15, 'natural reference')
            select(0)
            reference[index] = fetch(items()[0])
        assert len({hashlib.sha256(body[44:]).digest() for body in reference.values()}) == 8
        print('PASS natural PCM references: eight distinct tracks', flush=True)
        cli('stop', mac)
        wait(lambda: not items(), 10, 'reference finished')
        cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
        wait(lambda: len(items()) == 4, 15, 'soak initial window')
        select(0)
        time.sleep(.5)
        mark, before, stops = len(log()), len(proxy.records), len(sonos.actions('Stop'))
        expected, index, steps, pause_cycles = 0, 0, 0, 0
        natural_at = [40, 90, 140]
        def mutations():
            writes = [r['request'] for r in proxy.records[before:]
                      if ' status ' not in r['request'] and not r['request'].endswith(' ?')
                      and r['request'] != 'listen 1']
            assert all(' playlist index ' in wire for wire in writes), writes
            return writes
        def settle(target, offset=False, grace=.4):
            wait(lambda: int(cli('playlist index ?', mac)[-1]) == target and bridge_at(target) and
                 any(i['metadata']['title'] == f'Title & {target}' and
                     i['uri'] == sonos.position['TrackURI'] for i in items()),
                 10, f'seed {seed} step {steps} index {target}')
            # Include producer pairing and edit convergence, not just the
            # renderer's instantaneous row assignment.
            time.sleep(grace)
            body = fetch(row(target), True)
            if offset:
                assert body[44:] in reference[target][44:], ('seek PCM', seed, steps)
            else:
                assert body == reference[target][:len(body)], ('PCM', seed, steps)
            text = log()[mark:]
            assert 'lazy abandoned' not in text and 'lazy deadline' not in text, (seed, steps, text[-6000:])
            assert 'rebuild reason=clear' not in text and 'relay fault=' not in text, (seed, steps, text[-6000:])
            assert sonos.transport_state == 'PLAYING' and len(sonos.actions('Stop')) == stops
            assert len(mutations()) == expected, (seed, steps, mutations(), expected)
        while steps < 200:
            if natural_at and steps >= natural_at[0] and index < 7:
                natural_at.pop(0)
                fetch(row(index))
                with sonos.lock:
                    sonos.position['RelTime'] = '00:00:23'
                target = index + 1
                def prepared():
                    url = urlsplit(row(target)['uri'])
                    conn = http.client.HTTPConnection(url.netloc, timeout=3)
                    try:
                        conn.request('HEAD', url.path)
                        reply = conn.getresponse()
                        reply.read()
                        return reply.status == 200
                    finally:
                        conn.close()
                wait(prepared, 10, 'natural next producer ready without activating GET')
                audible(target)
                steps += 1
                index = target
                settle(index)
                print(f'SOAK seed={seed} step={steps} op=natural index={index} commands={len(mutations())} expected={expected}', flush=True)
                continue
            operation = rng.choice(('get', 'poll', 'lms-next', 'lms-back', 'jump', 'seek', 'pause', 'burst'))
            available = [int(i['metadata']['title'].split()[-1]) for i in items()]
            if operation == 'burst' and (200-steps < 2 or len([n for n in available if n > index]) < 2):
                operation = 'poll'
            if operation in ('get', 'poll') and not any(n != index for n in available):
                operation = 'jump'
            if operation == 'pause':
                cli('pause 1', mac)
                wait(lambda: sonos.transport_state == 'PAUSED_PLAYBACK' and
                     json.loads((base / ('stream-pacing-' + b'Study'.hex() + '.json')).read_text())['playback']['state'] == 'PAUSED', 5, 'soak pause')
                cli('pause 0', mac)
                wait(lambda: sonos.transport_state == 'PLAYING', 5, 'soak resume')
                pause_cycles += 1
                # Pause cycles are additional to the 200 navigation steps.
                # Resume retains the current seek offset, if any.
                settle(index, True)
            elif operation == 'seek':
                seek_mark = len(log())
                cli('time 6', mac)
                wait(lambda: 'kind=seek' in log()[seek_mark:], 10, 'soak seek')
                time.sleep(.4)
                steps += 1
                settle(index, True)
            elif operation == 'burst' and len([n for n in available if n > index]) >= 2:
                upcoming = [n for n in available if n > index]
                count = min(rng.randint(2, 4), 200-steps)
                targets = (upcoming[:2] * 2)[:count] if count == 4 else upcoming[:count]
                burst_start = time.monotonic()
                last_arrival = burst_start
                for target in targets:
                    last_arrival = time.monotonic()
                    audible(target)
                    body = fetch(row(target), True)
                    assert body == reference[target][:len(body)], (seed, steps, target)
                    expected += 1
                    steps += 1
                    index = target
                    settle(index, grace=0)
                elapsed = last_arrival - burst_start
                print(f'BURST seed={seed} moves={len(targets)} elapsed_ms={elapsed*1000:.0f}', flush=True)
                time.sleep(.4)
            else:
                if operation in ('get', 'poll', 'burst') and any(n != index for n in available):
                    target = rng.choice([n for n in available if n != index])
                    if operation == 'get' and target > index:
                        body = fetch(row(target), True)
                        assert body == reference[target][:len(body)]
                        audible(target)
                    else:
                        audible(target)
                    expected += 1
                else:
                    target = min(7, index+1) if operation == 'lms-next' else max(0, index-1) if operation == 'lms-back' else rng.randrange(8)
                    # Previous at index zero is a restart, never a clear.
                    nav_mark = len(log())
                    cli('playlist index ' + str(target), mac)
                    wait(lambda: any('window lms-nav ' in line and f'to_index={target} ' in line
                         for line in log()[nav_mark:].splitlines()), 10, 'controller navigation settled')
                steps += 1
                index = target
                settle(index)
            print(f'SOAK seed={seed} step={steps} op={operation} index={index} commands={len(mutations())} expected={expected}', flush=True)
        print(f'PASS SOAK seed={seed} steps=200 commands={len(mutations())} speaker_moves={expected} abandoned=0 clears=0 stops=0 byte_identity=yes pause_cycles={pause_cycles}', flush=True)
    else:
        raise AssertionError(case)
    cli('stop', mac)
    wait(lambda: not items(), 10, 'explicit stop release')
