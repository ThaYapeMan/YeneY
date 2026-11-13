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
    def natural_pcm(index):
        if index == 0 and os.environ.get('YENEY_NAV_MEDIA', '').startswith('converted'):
            import numpy as np
            import subprocess
            source = next((base/'library').glob('nav-0.*'))
            decoded = subprocess.check_output([str(Path(__file__).resolve().parents[1]/'third_party/yeney-core/mp4-position-test'),
                                               'convert', 'full'], input=source.read_bytes())
            values = np.frombuffer(decoded[32:], dtype='<i4').astype(np.int64)
            values = (values * 39424) // 65536
            values = np.where(values >= 0, (values+128)//256, -((-values+128)//256))
            return values.astype('<i4').view(np.uint8).reshape(-1,4)[:,:3].tobytes()
        source = (base / f'input-{index}.wav').read_bytes()[44:]
        if os.environ.get('YENEY_NAV_MEDIA'):
            import numpy as np
            values = np.frombuffer(source, dtype='<i2').astype(np.int64) * 39424
            result = (values >> 8).astype('<i4').view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
            if os.environ.get('YENEY_NAV_MEDIA') == 'trim': result = result[113*6:-219*6]
            return result
        return b''.join(b'\0' + source[n:n+2] for n in range(0, len(source), 2))
    def declared(item):
        url = urlsplit(item['uri'])
        conn = http.client.HTTPConnection(url.netloc, timeout=3)
        try:
            conn.request('HEAD', url.path)
            response = conn.getresponse()
            response.read()
            return response.status == 200
        finally:
            conn.close()
    def row(index):
        return next(i for i in items() if i['metadata']['title'] == f'Title & {index}')
    def audible(index):
        select(items().index(row(index)))
    def natural_ready(complete=False):
        settled = None
        def decoder_ready():
            nonlocal settled
            text = log()
            opening = text.rfind('core: queue command strm s')
            audio = re.findall(r'queue decode occurrence=(\d+) first audio', text[opening:])
            if ' first audio' not in text[opening:] or (complete and
                    (not audio or f'queue item-end occurrence={audio[-1]} ' not in text[opening:])):
                settled = None
                return False
            if settled is None or settled[0] != opening:
                settled = (opening, time.monotonic())
            return time.monotonic() - settled[1] >= .2
        wait(decoder_ready, 10, 'stream opening settled before controller seek')
    cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
    wait(lambda: len(items()) == len(ids), 15, 'eight-item initial window')
    select(0)
    mark = len(log())
    if case == 'D6':
        fetch(row(0), True)
        natural_ready()
        Path(os.environ['YENEY_TEST_WORKER_TRIGGER']).touch()
        cli('time 12', mac)
        wait(lambda: 'queue worker fault=injected programming error' in log(), 10, 'worker boundary diagnostic')
        wait(lambda: not items(), 10, 'fault releases owned queue')
        assert c['bridge'].poll() is None, 'programming fault terminated bridge'
        assert sonos.crossfade and sonos.verified_save
        assert 'terminate' not in log()
        assert cli('version ?')[-1].startswith('9.1.1')
        print('PASS D6 programming exception inside worker: room released, process alive, LMS available; independent worker isolation covered by seek-worker-test', flush=True)
        return
    if case.startswith('S') and case != 'SOAK' or case == 'D1':
        if case in ('S2', 'S3'):
            fetch(row(0), True)
            wait(c['tap'].http_gate.held.is_set, 5, 'natural producer held')
            time.sleep(.5)
            reference_pcm = natural_pcm(0)
            reference = None
        else:
            reference = fetch(row(0))
            natural_ready()
        target = row(0)
        uri, duration = target['uri'], target['resource']['duration']
        mark, before = len(log()), len(proxy.records)
        seek_count = len(sonos.actions('Seek'))
        position = (5 if os.environ.get('YENEY_NAV_MEDIA') else 1) if case == 'S2' else 60 if case == 'S4c' else 12
        edits = (len(sonos.actions('AddURIToQueue')), len(sonos.actions('RemoveTrackRangeFromQueue')))
        if case == 'D1':
            retained = http.client.HTTPConnection(urlsplit(uri).netloc, timeout=8)
            c['extra_remotes'].append(retained)
            retained.request('GET', urlsplit(uri).path)
            response = retained.getresponse()
            header = response.read(44)
            assert response.status == 200 and header == reference[:44]
            gate = c['tap'].http_gate
            def verify_retained_before_switch():
                wait(lambda: 'decision=fallback reason=landed-pcm-mismatch' in log()[mark:], 5, 'mismatch decision before speaker switch')
                assert sonos.position['TrackURI'] == uri
                assert header + response.read() == reference, 'failure dropped or changed the retained HTTP reader'
                retained.close()
                gate.retained_verified.set()
            gate.on_corruption = lambda: setattr(sonos, 'browse_hook', verify_retained_before_switch)
            gate.corrupt_next.set()
        seek_issued = time.monotonic()
        if case != 'S4d': cli(f'time {position}', mac)
        if case != 'S4d': wait(lambda: 'kind=seek' in log()[mark:], 10, 'LMS seek reconciled')
        time.sleep(.5)
        text = log()[mark:]
        if case == 'D1':
            assert c['tap'].http_gate.corruptions == 1
            assert c['tap'].http_gate.retained_verified.is_set(), 'retained HTTP reader was not verified before the switch'
            evidence = next(line for line in text.splitlines() if 'decision=fallback reason=landed-pcm-mismatch' in line)
            assert all(field in evidence for field in ('first_differing_frame=', 'landed_frame=', 'wav_origin=', 'retained_gain=', 'producer_gain=')), evidence
            assert len(sonos.actions('Seek')) == seek_count + 1 and sonos.position['TrackURI'] != uri
            assert not sonos.actions('Stop') and c['bridge'].poll() is None
            assert 'worker fault=' not in log() and 'relay fault=' not in log()
            wait(lambda: abs(float(cli('time ?', mac)[-1]) - position) <= .5, 5, 'mismatch fallback clock within 500 ms')
            print(text, flush=True)
            print('PASS D1 fake corrupts one ALAC frame: evidence, live HTTP reader byte-identical before one speaker switch, zero Stop, process alive, LMS clock within 500 ms', flush=True)
            return
        if 'nonaligned' in os.environ.get('YENEY_NAV_MEDIA', ''):
            landing = re.search(r'first sample=(\d+)', text)
            assert landing and int(landing[1]) % 4, text
            print('E6 nonmultiple landed_source=' + landing[1], flush=True)
            assert 'decision=fallback reason=landed-frame-unknown' in text, text
            assert sonos.position['TrackURI'] != uri
            assert len(sonos.actions('Seek')) == seek_count + 1
            assert not sonos.actions('Stop') and c['bridge'].poll() is None
            print('PASS E6 nonmultiple FLAC block landing: unknown fallback, one switch, process alive', flush=True)
            return
        if case == 'S3':
            assert sonos.position['TrackURI'] != uri
            assert len(sonos.actions('Seek')) == seek_count + 1
            assert not sonos.actions('Stop')
            assert fetch(row(0))[44:] in reference_pcm
            print('S3 fallback behavior: single switch, one renderer Seek, PCM suffix, no stop/drop', flush=True)
            assert 'decision=fallback reason=prefix-unavailable' in text, text
            print(text, flush=True)
            print('PASS S3 fallback prefix-unavailable: single switch, no stop/drop (baseline red only on new diagnostic)', flush=True)
            return
        if case != 'S4d':
            assert 'decision=in-item' in text, text
            assert 'decision=fallback' not in text, text
            if os.environ.get('YENEY_NAV_MEDIA') == 'trim':
                evidence = re.search(r'landed_frame=(\d+) warmup=\d+ wav_origin=(\d+) attach_offset=(\d+)', text)
                assert evidence and int(evidence[2]) == 113 and int(evidence[3]) == int(evidence[1]) - 113, text
        assert sonos.position['TrackURI'] == uri, ('seek replaced audible URI', uri, sonos.position)
        assert edits == (len(sonos.actions('AddURIToQueue')), len(sonos.actions('RemoveTrackRangeFromQueue')))
        assert row(0)['resource']['duration'] == duration
        assert 're-produced' not in text and 'window replace' not in text, text
        seeks = sonos.actions('Seek')[seek_count:]
        assert len(seeks) == (0 if case == 'S4d' else 1) and all(s[1]['Unit'] == 'REL_TIME' and s[1]['Target'] == f'00:{position//60:02}:{position%60:02}' for s in seeks), seeks
        if case != 'S4d': wait(lambda: abs(float(cli('time ?', mac)[-1]) - position) <= 2, 5, 'speaker RelTime equals LMS time')
        if os.environ.get('YENEY_NAV_MEDIA', '').startswith('converted') or os.environ.get('YENEY_NAV_MEDIA', '').startswith('private-'):
            import struct
            evidence = {}
            def clock_equation():
                packets = [r for r in c['tap'].records if r.get('event') == 'STMt' and r['when'] >= seek_issued]
                if not packets: return False
                stat = packets[-1]
                elapsed = struct.unpack_from('!I', bytes.fromhex(stat['body']), 43)[0] / 1000
                lms = float(cli('time ?', mac)[-1])
                expected_time = position + elapsed + time.monotonic() - stat['when']
                evidence.update(offset=position, stat_elapsed=elapsed, lms=lms, expected=expected_time)
                return abs(lms - expected_time) <= .5
            wait(clock_equation, 8, 'converted clock equation within 500 ms')
            print('R-B3 clock equation ' + repr(evidence), flush=True)
        body = fetch(row(0))
        assert (body == reference if reference is not None else body[44:] == reference_pcm), 'immutable full WAV changed'
        offset = position * int.from_bytes(body[24:28], "little") * 6 + 44
        conn = http.client.HTTPConnection(urlsplit(uri).netloc, timeout=8)
        try:
            conn.request('GET', urlsplit(uri).path, headers={'Range': f'bytes={offset}-'})
            response = conn.getresponse()
            ranged = response.read()
            assert response.status == 206
            assert response.getheader('Content-Range') == f'bytes {offset}-{len(body)-1}/{len(body)}'
            assert int(response.getheader('Content-Length')) == len(body)-offset
            assert ranged == body[offset:], 'Range PCM differs across seek attach'
        finally:
            conn.close()
        assert not [r for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        if case in ('S4', 'S4b', 'S4c', 'S4d'):
            positions = [30, 0] if case == 'S4d' else [60 if case == 'S4b' else 5 if case == 'S4c' else 0]
            for app_position in positions:
                app_mark, app_before = len(log()), len(proxy.records)
                app_seeks = len(sonos.actions('Seek'))
                app_plays = len(sonos.actions('Play'))
                read_mark = len(sonos.audible_reads)
                app_stamp = time.monotonic()
                sonos.soap_delay = .6
                with sonos.lock:
                    sonos.position['RelTime'] = f'{app_position//3600:02}:{app_position//60%60:02}:{app_position%60:02}'
                sonos.read_audio(0)
                assert fetch(row(0), True) == body[:44+4096], 'app PCM request dropped or changed'
                expected = 1 if case in ('S4', 'S4c') else 0
                if not expected:
                    wait(lambda: abs(float(cli('time ?', mac)[-1]) - app_position) <= 1.25, 8, 'STAT-only app clock')
                    print(f'{case} STAT-only clock reaches {app_position} seconds', flush=True)
                wait(lambda: 'queue speaker-seek ' in log()[app_mark:], 6, 'app position classified')
                if expected:
                    wait(lambda: 'decision=paired' in log()[app_mark:] or 'decision=in-item' in log()[app_mark:], 12, 'rewind paired')
                    command = next(r for r in proxy.records[app_before:] if f' time {app_position}' in r['request'])
                    paired_stamp = time.monotonic()
                    chain = [r[0] for r in sonos.requests if command['when'] <= r[3] <= paired_stamp and r[0] in ('Browse', 'GetZoneGroupState')]
                    print('E1 rewind SOAP chain=' + repr(chain), flush=True)
                    print(f'E1 {case} time_command_to_paired_ms={(time.monotonic()-command["when"])*1000:.1f}', flush=True)
                time.sleep(1.2)
                speaker_seconds = sum(int(v)*w for v, w in zip(sonos.position['RelTime'].split(':'), (3600,60,1)))
                assert abs(float(cli('time ?', mac)[-1]) - speaker_seconds) <= 1.25
                commands = [r['request'] for r in proxy.records[app_before:] if
                            re.search(r' (time|playlist index) [^?]', r['request'])]
                assert len(commands) == expected, commands
                if expected:
                    assert f'reason=speaker-rewind command=time {app_position}' in log()[app_mark:]
                    assert 'decision=paired' in log()[app_mark:] or 'decision=in-item' in log()[app_mark:]
                else:
                    assert 'decision=stat' in log()[app_mark:]
                print('E1 renderer_seeks=' + str(len(sonos.actions('Seek'))-app_seeks) + ' renderer_plays=' + str(len(sonos.actions('Play'))-app_plays) + ' audible_reads=' + repr(sonos.audible_reads[read_mark:]), flush=True)
                assert len(sonos.actions('Seek')) == app_seeks, sonos.actions('Seek')[app_seeks:]
                assert len(sonos.actions('Play')) == app_plays, sonos.actions('Play')[app_plays:]
                if expected: assert not chain, chain
                time.sleep(max(0, 10-(time.monotonic()-app_stamp)))
                assert len([r for r in sonos.audible_reads[read_mark:] if r[1] == uri and r[2] == 0]) == 1, sonos.audible_reads[read_mark:]
                def clock_follows():
                    speaker_seconds = sum(int(v)*w for v, w in zip(sonos.position['RelTime'].split(':'), (3600,60,1)))
                    return abs(float(cli('time ?', mac)[-1]) - speaker_seconds) <= 1.25
                wait(clock_follows, 8, 'advancing rewind clock within existing tolerance')
                assert len(sonos.actions('Seek')) == app_seeks, sonos.actions('Seek')[app_seeks:]
                assert len(sonos.actions('Play')) == app_plays, sonos.actions('Play')[app_plays:]
                sonos.soap_delay = 0
                assert sonos.position['TrackURI'] == uri
                assert edits == (len(sonos.actions('AddURIToQueue')), len(sonos.actions('RemoveTrackRangeFromQueue')))
                assert fetch(row(0)) == body
                assert not sonos.actions('Stop')
                stat = next(r for r in reversed(c['tap'].records) if r['op'] == 'STAT')
                print(case + ' clock evidence: speaker_rel_time=' + sonos.position['RelTime'] +
                      ' lms_time=' + cli('time ?', mac)[-1] + ' STAT=' + stat['body'], flush=True)
                print(log()[app_mark:], flush=True)
        if case == 'S5':
            nav_before = len(proxy.records)
            audible(1)
            wait(lambda: bridge_at(1), 10, 'app Next')
            time.sleep(1)
            back_mark, back_seeks = len(log()), len(sonos.actions('Seek'))
            audible(0)
            wait(lambda: bridge_at(0) and 'decision=paired' in log()[back_mark:], 10, 'Back paired frame-zero parent')
            assert len(sonos.actions('Seek')) == back_seeks
            assert sonos.position['TrackURI'] == uri and fetch(row(0)) == reference
            wait(lambda: float(cli('time ?', mac)[-1]) <= 2, 5, 'Back starts at zero')
            nav_commands = [r['request'] for r in proxy.records[nav_before:] if re.search(r' (time|playlist index) [^?]', r['request'])]
            assert nav_commands == [mac + ' playlist index 1', mac + ' playlist index 0'], nav_commands
            assert 're-produced' not in log()[back_mark:]
        print(text, flush=True)
        result = {
            'S4': 'LMS 12s then app Back: one time 0 reason=speaker-rewind, frame-zero paired, zero renderer Seek, stable URI/WAV, clock within tolerance, no stop/drop',
            'S4b': 'LMS 12s then app 60s: zero commands, decision=stat, stable URI/WAV, clock within tolerance',
            'S4c': 'LMS 60s then app 5s: one time 5 reason=speaker-rewind, landed-frame in-item attach, zero renderer Seek, stable URI/WAV, clock within tolerance',
            'S4d': 'offset zero app 30s then Back: zero commands throughout, zero renderer Seek, stable URI/WAV, clock within tolerance',
            'S5': 'LMS seek then app Next/Back: frame-zero paired, zero reproduction, one command per navigation, zero renderer Seek on Back, stable WAV',
        }.get(case, 'in-item LMS seek: stable URI/duration, one REL_TIME, zero reproduction, clock and WAV/Range identity')
        print(f'PASS {case} {result}', flush=True)
    elif case in ('A1', 'A2', 'A3'):

        reference0 = fetch(row(0))
        def ready(index):
            u = urlsplit(row(index)['uri'])
            conn = http.client.HTTPConnection(u.netloc, timeout=3)
            try:
                conn.request('HEAD', u.path)
                reply = conn.getresponse()
                reply.read()
                return reply.status == 200
            finally:
                conn.close()
        wait(lambda: ready(1), 10, 'natural decode-ahead declaration')
        target = row(1)
        if case == 'A2':
            wait(c['tap'].http_gate.held.is_set, 5, 'unfinished natural decode-ahead held after first encoded prefix')
            assert c['tap'].http_gate.prefix_bytes == c['tap'].http_gate.prefix_limit
        if case != 'A2':
            reference = fetch(target)
        else:
            reference = None
        if case == 'A3':
            nav_before = len(proxy.records)
            audible(1)
            wait(lambda: bridge_at(1), 10, 'Next before retained Back')
            time.sleep(1)
            target, reference = row(0), reference0
        target_index = 0 if case == 'A3' else 1
        mark, before, seeks = len(log()), len(proxy.records), len(sonos.actions('Seek'))
        select(items().index(target))
        switched = sonos.position['TrackURI']
        switch_mark = len(sonos.position_history)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == target_index and bridge_at(target_index),
             10, 'speaker navigation paired')
        time.sleep(2)
        text = log()[mark:]
        assert 'target re-produced' not in text, text
        assert len(sonos.actions('Seek')) == seeks, sonos.actions('Seek')[seeks:]
        assert sonos.position['TrackURI'] == switched, (switched, sonos.position)
        assert not sonos.position_history[switch_mark:], sonos.position_history[switch_mark:]
        assert 'decision=paired' in text, text
        body = fetch(row(target_index))
        if reference is not None:
            assert body == reference, 'paired audio differs from natural WAV'
        else:
            expected_pcm = natural_pcm(1)
            assert body[44:] == expected_pcm, 'continued PCM differs from natural 16-to-24-bit production'
        commands = [r['request'] for r in proxy.records[before:]
                    if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert commands == [mac + f' playlist index {target_index}'], commands
        if case == 'A2':
            assert c['tap'].http_gate.requests >= 3, 'fresh producer must continue the held natural stream'
            print(f'A2 stimulus: natural producer held after {c["tap"].http_gate.prefix_bytes} encoded bytes; fresh producer continued same WAV', flush=True)
        print(next(line for line in text.splitlines() if 'window pairing ' in line), flush=True)
        print(f'PASS {case} paired retained frame-zero target: no re-production, no Seek, stable audible URI, one command, byte identity', flush=True)
    elif case == 'F1':
        before = len(proxy.records)
        # Queue a second live request while the first selection is settling.
        targets = [row(1), row(2), row(3)]
        preparation_seeks = len(sonos.actions('Seek'))
        with concurrent.futures.ThreadPoolExecutor(3) as pool:
            futures = []
            for target in targets:
                futures.append(pool.submit(fetch, target, True))
                time.sleep(.08)
            for f in futures:
                f.result()
        assert len(sonos.actions('Seek')) == preparation_seeks, 'lazy PCM preparation must not select the renderer'
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
            wait(lambda: len(items()) == len(ids), 15, 'targeted fresh window')
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
        wait(lambda: len(items()) == len(ids), 15, 'lazy Back window')
        select(0)
        cli('playlist index 5', mac)
        wait(lambda: bridge_at(5) and len(items()) == len(ids), 15, 'lazy Back starts at five')
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
        assert sonos.position['RelTime'] == '00:00:00', 'LMS Previous must reset the same retained URI to frame zero'
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
        assert len(items()) == len(ids) and sonos.transport_state == 'PLAYING', log()[mark:]
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

    elif case in ('F3', 'B1', 'B2'):
        prefix = fetch(row(0), True)
        wait(c['tap'].http_gate.held.is_set, 5, 'fallback producer held before seek point')
        source = (base / 'input-0.wav').read_bytes()[44:]
        reference = prefix[:44] + b''.join(b'\0' + source[n:n+2] for n in range(0, len(source), 2))
        time.sleep(.5)
        cli('time 12', mac)
        wait(lambda: 'kind=seek' in log()[mark:], 15, 'track-zero seek')
        assert 'decision=fallback reason=prefix-unavailable' in log()[mark:], log()[mark:]
        partial = row(0)['uri']
        # Finish the partial so it really is retained history. Let natural
        # preparation declare its successor before the controller move, so
        # this setup cannot cancel a just-opening decoder connection.
        prepare_mark = len(log())
        fetch(row(0))
        wait(lambda: re.search(r'core: queue FLAC occurrence=\d+ exact remaining=\d+ first sample=0',
                               log()[prepare_mark:]), 8, 'natural successor declaration before F3 setup move')
        fetch(row(1))
        next_mark = len(log())
        cli('playlist index 1', mac)
        wait(lambda: any(i['metadata']['title'] == 'Title & 1' and i['uri'] == sonos.position['TrackURI'] for i in items()), 15, 'navigate away from seek')
        wait(lambda: 'to_index=1 action=slide' in log()[next_mark:] and
             any(i['metadata']['title'] == 'Title & 0' and i['uri'] != partial for i in items()),
             8, 'Next window committed with deferred frame-zero history')
        before = len(proxy.records)
        mark = len(log())
        seeks = len(sonos.actions('Seek'))
        switch_mark = len(sonos.position_history)
        if case == 'B2':
            cli('playlist index 0', mac)
        else:
            audible(0)
            if case == 'B1':
                assert sonos.position['TrackURI'] != partial, 'speaker switched to retained seek partial before re-production'
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == 0, 8, 'speaker Back to partial')
        wait(lambda: any(i['metadata']['title'] == 'Title & 0' and i['uri'] != partial and i['uri'] == sonos.position['TrackURI'] for i in items()),
             8, 'fresh frame-zero target replaces seek partial')
        body = fetch(row(0))
        assert body == reference, 'fresh frame-zero WAV differs from natural production'
        commands = [r['request'] for r in proxy.records[before:]
                    if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert commands == ([] if case == 'B2' else [mac + ' playlist index 0']), commands
        assert sonos.transport_state == 'PLAYING', log()[mark:]
        assert 'window target re-produced lms_index=0 origin_ms=0 lms_commands=' + ('0' if case == 'B2' else '1') in log()[mark:]
        if case != 'F3':
            switches = [uri for _, uri in sonos.position_history[switch_mark:] if uri]
            assert switches == [row(0)['uri']] and partial not in switches, switches
            assert 'reason=seek-partial' in log()[mark:]
            assert len(sonos.actions('Seek')) == seeks + (1 if case == 'B2' else 0), sonos.actions('Seek')[seeks:]
        assert 'to_index=0 action=slide history=0 lms_commands=' + ('0' if case == 'B2' else '1') + ' kind=previous' in log()[mark:]
        print(next(line for line in log()[mark:].splitlines() if 'target re-produced' in line), flush=True)
        print(f'PASS {case} seek partial re-produced reason=seek-partial: frame-zero WAV, commands={len(commands)}, renderer_seeks={len(sonos.actions("Seek"))-seeks}, no partial switch, no stop', flush=True)
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
        wait(lambda: len(items()) == len(ids), 15, 'soak initial window')
        select(0)
        time.sleep(.5)
        mark, before, stops = len(log()), len(proxy.records), len(sonos.actions('Stop'))
        queue_mark = len(sonos.requests)
        expected, index, steps, pause_cycles = 0, 0, 0, 0
        natural_at = [40, 90, 140]
        seek_partials = set()
        lms_offset = 0
        app_rewinds, app_stats, app_zero, app_above = 0, 0, 0, 0
        expected_uri = None
        pairing_uri = None
        action_mark = len(log())
        action_history_mark = len(sonos.position_history)
        def mutations():
            writes = [r['request'] for r in proxy.records[before:]
                      if ' status ' not in r['request'] and not r['request'].endswith(' ?')
                      and r['request'] != 'listen 1']
            assert all(' playlist index ' in wire or re.search(r' time [0-9]+$', wire) for wire in writes), writes
            return writes
        def settle(target, offset=False, grace=.4):
            initial_uri = sonos.position['TrackURI']
            history_mark = len(sonos.position_history)
            wait(lambda: int(cli('playlist index ?', mac)[-1]) == target and bridge_at(target) and
                 any(i['metadata']['title'] == f'Title & {target}' and
                     i['uri'] == sonos.position['TrackURI'] for i in items()),
                 10, f'seed {seed} step {steps} index {target}')
            # Include producer pairing and edit convergence, not just the
            # renderer's instantaneous row assignment.
            time.sleep(grace)
            if expected_uri is not None and not offset:
                assert sonos.position['TrackURI'] == expected_uri, ('audible target changed', seed, steps, expected_uri, sonos.position)
            if pairing_uri is not None and not offset:
                assert 'target re-produced' not in log()[action_mark:], ('frame-zero same-source re-produced', seed, steps, log()[action_mark:])
            stable_uri = sonos.position['TrackURI']
            stable_mark = len(sonos.position_history)
            body = fetch(row(target), True)
            assert not sonos.position_history[stable_mark:], ('audible swap', seed, steps, sonos.position_history[stable_mark:])
            switches = [uri for _, uri in sonos.position_history[action_history_mark:] if uri]
            assert len(switches) <= 1, ('multiple navigation switches', seed, steps, switches)
            if initial_uri == stable_uri:
                assert not sonos.position_history[history_mark:], ('audible swap after switch', seed, steps, sonos.position_history[history_mark:])
            if offset:
                assert body[44:] in reference[target][44:], ('seek PCM', seed, steps)
            else:
                assert body == reference[target][:len(body)], ('PCM', seed, steps)
            text = log()[mark:]
            assert len(items()) == min(len(ids), 500), ('static mirror row count', seed, steps)
            replacements = text.count('reason=seek-partial edits=2')
            edits = [r for r in sonos.requests[queue_mark:] if r[0] in
                     ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
            assert len(edits) == replacements * 2, ('navigation edits outside partial exception', seed, steps, len(edits), replacements)
            for line in text.splitlines():
                if 're-produced' in line:
                    assert re.search(r'reason=(seek-partial|source-mismatch|incomplete)( |$)', line), line
            assert 'lazy abandoned' not in text and 'lazy deadline' not in text, (seed, steps, text[-6000:])
            assert 'rebuild reason=clear' not in text and 'relay fault=' not in text, (seed, steps, text[-6000:])
            assert sonos.transport_state == 'PLAYING' and len(sonos.actions('Stop')) == stops
            assert len(mutations()) == expected, (seed, steps, mutations(), expected)
        # Every seed proves the whole offset rule before its randomized workload.
        contract = [('seek', 12), ('app-position', 18), ('app-position', 13),
                    ('app-position', 5), ('app-position', 0),
                    ('app-position', 18), ('app-position', 0)]
        while steps < 200:
            expected_uri = None
            pairing_uri = None
            action_mark = len(log())
            action_history_mark = len(sonos.position_history)
            if natural_at and steps >= natural_at[0] and index < 7:
                natural_at.pop(0)
                fetch(row(index))
                duration = sum(float(part) * weight for part, weight in zip(row(index)['resource']['duration'].split(':'), (3600, 60, 1)))
                near_end = max(0, int(duration) - 1)
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
                status_path = base / ('stream-pacing-' + b'Study'.hex() + '.json')
                producer = json.loads(status_path.read_text())['playback']['occurrence']
                wait(lambda: f'core: queue item-end occurrence={producer} ' in log(), 10,
                     'current natural producer completed, including paired prefix')
                prefetch_position = max(0, int(duration) - 15)
                with sonos.lock:
                    sonos.position['RelTime'] = f'{prefetch_position//3600:02}:{prefetch_position//60%60:02}:{prefetch_position%60:02}'
                wait(lambda: re.search((r'queue production occurrence=' + str(producer + 1) + r' content_frame=0 ' if os.environ.get('YENEY_NAV_MEDIA') else r'core: queue FLAC occurrence=' + str(producer + 1) + r' .* first sample=0'), log()),
                     10, 'fresh natural successor coordinate')
                wait(prepared, 10, 'natural next producer ready without activating GET')
                with sonos.lock:
                    sonos.position['RelTime'] = f'{near_end//3600:02}:{near_end//60%60:02}:{near_end%60:02}'
                wait(lambda: abs(float(cli('time ?', mac)[-1]) - near_end) <= 1.25, 5, 'outgoing final-second clock observed before boundary')
                expected_uri = row(target)['uri']
                pairing_uri = expected_uri if declared(row(target)) else None
                audible(target)
                steps += 1
                index = target
                lms_offset = 0
                settle(index)
                print(f'SOAK seed={seed} step={steps} op=natural index={index} commands={len(mutations())} expected={expected}', flush=True)
                continue
            scripted = contract[steps] if steps < len(contract) else None
            operation = scripted[0] if scripted else rng.choice(('get', 'poll', 'lms-next', 'lms-back', 'jump', 'seek', 'app-position', 'pause', 'burst'))
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
            elif operation == 'app-position':
                fetch(row(index))
                previous_position = sum(int(n) * weight for n, weight in zip(sonos.position['RelTime'].split(':'), (3600, 60, 1)))
                position = scripted[1] if scripted else rng.choice([n for n in (0, 2, 18) if abs(n-previous_position) > 2])
                app_uri = sonos.position['TrackURI']
                app_mark, app_before = len(log()), len(mutations())
                app_seeks = len(sonos.actions('Seek'))
                app_plays = len(sonos.actions('Play'))
                sonos.soap_delay = .6
                rewind = position * 1000 < lms_offset
                app_rewinds += int(rewind)
                app_stats += int(not rewind)
                app_zero += int(lms_offset == 0)
                app_above += int(not rewind and lms_offset > 0)
                with sonos.lock:
                    sonos.position['RelTime'] = f'00:00:{position:02}'
                wait(lambda: 'queue speaker-seek ' in log()[app_mark:], 6, 'soak app position classified')
                wait(lambda: abs(float(cli('time ?', mac)[-1]) - position) <= 1.25, 8, 'soak app clock')
                time.sleep(.6)
                expected += int(rewind)
                assert len(mutations()) - app_before == int(rewind), (seed, steps, log()[app_mark:])
                assert ('decision=time-command' if rewind else 'decision=stat') in log()[app_mark:]
                assert len(sonos.actions('Play')) == app_plays, sonos.actions('Play')[app_plays:]
                assert len(sonos.actions('Seek')) == app_seeks, (seed, steps, sonos.actions('Seek')[app_seeks:])
                assert sonos.position['TrackURI'] == app_uri
                sonos.soap_delay = 0
                if rewind: lms_offset = position * 1000
                steps += 1
                settle(index, True)
            elif operation == 'seek':
                # Finish the cached prefix and its current producer before
                # the random covered seek; cached audio can outlive a producer.
                fetch(row(index))
                natural_ready(complete=True)
                seek_uri = sonos.position['TrackURI']
                position = scripted[1] if scripted else rng.randrange(3, 18)
                seek_mark = len(log())
                cli(f'time {position}', mac)
                lms_offset = position * 1000
                wait(lambda: 'kind=seek' in log()[seek_mark:], 10, 'soak seek')
                time.sleep(.4)
                assert sonos.position['RelTime'] == f'00:00:{position:02}', (seed, steps, position, sonos.position)
                wait(lambda: abs(float(cli('time ?', mac)[-1]) - position) <= 1.25, 8, 'soak LMS seek clock')
                steps += 1
                settle(index, True)
                seek_text = log()[seek_mark:]
                assert sonos.position['TrackURI'] == seek_uri, ('covered seek replaced URI', seed, steps)
                assert 'seek-partial' not in seek_text, (seed, steps, seek_text)
                assert 'decision=fallback' not in seek_text, (seed, steps, seek_text)
                assert 'decision=in-item' in seek_text, (seed, steps, seek_text)
                for line in seek_text.splitlines():
                    if 'queue window seek ' in line:
                        assert 'decision=' in line, line
                        if 'decision=fallback' in line: assert 'reason=' in line, line
            elif operation == 'burst' and len([n for n in available if n > index]) >= 2:
                upcoming = [n for n in available if n > index]
                count = min(rng.randint(2, 4), 200-steps)
                targets = (upcoming[:2] * 2)[:count] if count == 4 else upcoming[:count]
                burst_start = time.monotonic()
                last_arrival = burst_start
                for target in targets:
                    last_arrival = time.monotonic()
                    action_mark = len(log())
                    action_history_mark = len(sonos.position_history)
                    expected_uri = row(target)['uri'] if row(target)['uri'] not in seek_partials else None
                    pairing_uri = expected_uri if declared(row(target)) else None
                    audible(target)
                    body = fetch(row(target), True)
                    assert body == reference[target][:len(body)], (seed, steps, target)
                    expected += 1
                    steps += 1
                    index = target
                    lms_offset = 0
                    settle(index, grace=0)
                elapsed = last_arrival - burst_start
                print(f'BURST seed={seed} moves={len(targets)} elapsed_ms={elapsed*1000:.0f}', flush=True)
                time.sleep(.4)
            else:
                if operation in ('get', 'poll', 'burst') and any(n != index for n in available):
                    target = rng.choice([n for n in available if n != index])
                    expected_uri = row(target)['uri'] if row(target)['uri'] not in seek_partials else None
                    pairing_uri = expected_uri if declared(row(target)) else None
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
                    expected_uri = next((i['uri'] for i in items() if i['metadata']['title'] == f'Title & {target}' and i['uri'] not in seek_partials and declared(i)), None)
                    pairing_uri = expected_uri
                    nav_mark = len(log())
                    cli('playlist index ' + str(target), mac)
                    wait(lambda: any('window lms-nav ' in line and f'to_index={target} ' in line
                         for line in log()[nav_mark:].splitlines()), 10, 'controller navigation settled')
                steps += 1
                index = target
                lms_offset = 0
                settle(index)
            print(f'SOAK seed={seed} step={steps} op={operation} index={index} commands={len(mutations())} expected={expected}', flush=True)
        assert app_rewinds and app_stats and app_zero and app_above, (seed, app_rewinds, app_stats, app_zero, app_above)
        assert 'relay fault=' not in log() and 'worker fault=' not in log(), log()
        assert c['bridge'].poll() is None, 'bridge exited during soak'
        replacements = log()[mark:].count('reason=seek-partial edits=2')
        print(f'PASS SOAK static_rows={len(items())} seek_partial_replacements={replacements} zero_navigation_edits=yes zero_relay_faults=yes zero_worker_faults=yes zero_process_exits=yes app_rewinds={app_rewinds} app_stats={app_stats} app_offset_zero={app_zero} app_above_positive_offset={app_above} app_position_command_counts=yes app_position_clock=yes covered_seek_uri=yes seed={seed} steps=200 commands={len(mutations())} speaker_commands={expected} abandoned=0 clears=0 stops=0 byte_identity=yes audible_swaps=0 reasoned_reproduction=yes valid_target_reproduction=0 pause_cycles={pause_cycles}', flush=True)
    else:
        raise AssertionError(case)
    cli('stop', mac)
    wait(lambda: not items(), 10, 'explicit stop release')
