# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Static playlist mirror acceptance against disposable LMS and renderer.

Latency profiles: device uses 30 ms SOAP, 400 ms transport transition and
50 ms GENA echo. Origin: owner's 12 November 2026 Study device journal,
7–45 ms consecutive SOAP spacing and 200–450 ms transitioning to playing.
strict retains the optional 600 ms per SOAP stress model; inherited retains
FULLQ_DELAY (600 ms default) and FULLQ_ALL_SOAP overrides from FULLQ-01b.
"""
import http.client
import json
import os
import time
from pathlib import Path
from urllib.parse import urlsplit


def run(c):
    cli, mac, wait, items, sonos, proxy = (c[k] for k in ('cli', 'mac', 'wait', 'items', 'sonos', 'proxy'))
    ids = sorted(c['window_ids'], key=lambda i: int(next(t['url'].rsplit('/', 1)[-1].split('.')[0].rsplit('-', 1)[-1] for t in c['tracks'] if t['id'] == i)))
    count = int(os.environ.get('YENEY_FULLQ_ROWS', '12'))
    playlist = [ids[n % len(ids)] for n in range(count)]
    profile = os.environ.get('YENEY_FULLQ_PROFILE', 'device')
    assert profile in ('device', 'strict', 'inherited'), profile
    sonos.soap_delay = float(os.environ.get('YENEY_FULLQ_DELAY', '.6'))
    sonos.all_soap_delay = sonos.soap_delay if os.environ.get('YENEY_FULLQ_ALL_SOAP', '1') == '1' else 0
    sonos.latency_profile(profile)
    print('MEASURE latency_profile=' + profile, flush=True)
    if os.environ.get('YENEY_FULLQ_CODEC') == 'alac':
        source = (c['base'] / 'library/fullq-0.m4a').read_bytes()
        print(f'MEASURE ALAC source_bytes={len(source)} moov_offset={source.find(b"moov")-4} mdat_offset={source.find(b"mdat")-4}', flush=True)
    def source_timing(trigger, stamp):
        starts = [r for r in c['tap'].records if r.get('op') == 'strm' and r.get('command') == 's' and trigger <= r['when'] <= stamp]
        for record in starts:
            body = bytes.fromhex(record['body'])
            print(f'MEASURE source_start codec={chr(body[2])} threshold_KiB={body[7]} trigger_to_strm={record["when"]-trigger:.3f} strm_to_audible={stamp-record["when"]:.3f}', flush=True)

    sonos.fullq_audio = True
    def calls_until(mark, stamp):
        return [r for r in sonos.requests[mark:] if r[3] <= stamp]
    def report_calls(operation, calls):
        print('SOAP ' + operation + ' ' + json.dumps([r[0] + (' ' + r[1]['Unit'] if r[0] == 'Seek' else '') for r in calls]), flush=True)
    album_mark = len(sonos.requests)
    sonos.reject_batches = os.environ.get('YENEY_FULLQ_REJECT_BATCHES') == '1'
    text = lambda: (c['base'] / 'bridge.log').read_text()
    def fetch(item):
        u = urlsplit(item['uri'])
        conn = http.client.HTTPConnection(u.netloc, timeout=10)
        try:
            conn.request('GET', u.path)
            reply = conn.getresponse()
            header = reply.read(44)
            first = reply.read(1)
            stamp = time.monotonic()
            body = header + first + reply.read(4096 - 1)
            assert reply.status == 200 and body[:4] == b'RIFF'
            return stamp, body
        finally:
            conn.close()
    def identity(index, body):
        import numpy as np
        source = (c['base'] / f'input-{index % len(ids)}.wav').read_bytes()[44:]
        # The generated source is signed 16-bit; the queue wire is signed 24-bit.
        expected = (np.frombuffer(source, dtype='<i2').astype('<i4') * 256).view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
        assert body[44:] == expected[:len(body)-44], ('PCM identity', index)
    if count > len(ids):
        cli('playlist clear', mac)
        for offset in range(0, count, len(ids)):
            cli('playlistcontrol cmd:add track_id:' + ','.join(playlist[offset:offset+len(ids)]), mac)
        assert int(cli('playlist tracks ?', mac)[-1]) == count
        started = time.monotonic()
        cli('playlist index 0', mac)
        cli('play', mac)
    else:
        started = time.monotonic()
        cli('playlistcontrol cmd:load track_id:' + ','.join(playlist), mac)
    wait(lambda: sonos.transport_state == 'PLAYING' and sonos.position['TrackURI'], 90, 'album playing')
    wait(lambda: bool(sonos.audible_reads), 15, 'album audible byte')
    stamp, body = sonos.audible_reads[0][0], sonos.audible_bodies[0]
    identity(0, body)
    codecs = [chr(bytes.fromhex(r['body'])[2]) for r in c['tap'].records if r.get('op') == 'strm' and r.get('command') == 's']
    print('MEASURE source_codec=' + os.environ.get('YENEY_FULLQ_CODEC', 'flac') + ' wire_codecs=' + repr(codecs), flush=True)
    if os.environ.get('YENEY_FULLQ_CODEC') == 'alac': assert codecs and all(codec == 'l' for codec in codecs)
    album_calls = calls_until(album_mark, stamp)
    report_calls('album_start', album_calls)
    cold_names = [r[0] for r in album_calls]
    cold_ok = (cold_names.count('GetZoneGroupState') == 1 and
               cold_names.count('SetCrossfadeMode') == 1 and
               cold_names.count('GetCrossfadeMode') == 2)
    print('C1 cold_start ' + ('PASS' if cold_ok else 'FAIL') + ' topology=' + str(cold_names.count('GetZoneGroupState')) + ' crossfade_get=' + str(cold_names.count('GetCrossfadeMode')) + ' crossfade_set=' + str(cold_names.count('SetCrossfadeMode')), flush=True)
    assert cold_ok, cold_names
    source_timing(started, stamp)
    album_latency = stamp - started
    lms_latencies, app_latencies = [], []
    print(f'MEASURE album_start audible_seconds={album_latency:.3f} rows={count}', flush=True)
    wait(lambda: len(items()) == count, 300, 'Q1 full static mirror')
    status_path = c['base'] / ('stream-pacing-' + b'Study'.hex() + '.json')
    def committed():
        return status_path.exists() and len(json.loads(status_path.read_text()).get('playback', {}).get('owned_item_uris', [])) == count
    wait(committed, 30, 'Q1 final Browse fence committed in bridge model')
    build = time.monotonic() - started
    assert [i['metadata']['title'] for i in items()] == [f'Title & {n % len(ids)}' for n in range(count)]
    assert int(sonos.position['Track']) == 1
    print(f'PASS Q1 rows={count} LMS order and current ordinal; build_seconds={build:.3f}', flush=True)
    assert len(sonos.items) == count and len({i['uri'] for i in items()}) == count
    assert len({sonos.item_ids[i['uri']] for i in items()}) == count
    assert all(i['resource']['duration'] == '0:00:24' for i in items())
    immutable = [i['uri'] for i in items()]
    cli('playlist repeat 2', mac)
    for index in (1, 2, 3, 2, 1, 9, count - 1, 0):
        mark, logmark = len(sonos.requests), len(text())
        start = time.monotonic()
        cli('playlist index ' + ('+1' if index == 0 else str(index)), mac)
        wait(lambda: sonos.position['TrackURI'] == immutable[index], 20, 'Q2 target')
        wait(lambda: any(r[1] == immutable[index] and r[0] >= start for r in sonos.audible_reads), 15, 'Q2 audible byte')
        pos = next(n for n, r in enumerate(sonos.audible_reads) if r[1] == immutable[index] and r[0] >= start)
        stamp, body = sonos.audible_reads[pos][0], sonos.audible_bodies[pos]
        identity(index, body)
        calls = calls_until(mark, stamp)
        report_calls(f'lms_index={index}', calls)
        assert not [r for r in sonos.requests[mark:] if r[0] in ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')], sonos.requests[mark:]
        assert [r[0] for r in calls if r[0] not in ('GetPositionInfo', 'GetTransportInfo', 'GetMediaInfo')] == ['Seek'], calls
        assert len([r for r in sonos.requests[mark:] if r[0] == 'Seek' and r[1]['Unit'] == 'TRACK_NR']) == 1, sonos.requests[mark:]
        wait(lambda: 'decision=paired' in text()[logmark:] or 'decision=re-produced' in text()[logmark:], 5, 'Q2 pairing decision')
        source_timing(start, stamp)
        lms_latencies.append(stamp - start)
        print(f'PASS Q2 index={index} audible_seconds={stamp-start:.3f} zero_edits=yes PCM=identical', flush=True)
    for index in (1, 0, 6, 1):
        before, mark, logmark = len(proxy.records), len(sonos.requests), len(text())
        start = time.monotonic()
        c['select_item'](index)
        sonos.read_audio()
        stamp, body = sonos.audible_reads[-1][0], sonos.audible_bodies[-1]
        identity(index, body)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == index, 10, 'Q3 LMS follows')
        wait(lambda: any(' playlist index ' in r['request'] and not r['request'].endswith(' ?')
                         for r in proxy.records[before:]), 10, 'Q3 selection command response recorded')
        report_calls(f'app_index={index}', calls_until(mark, stamp))
        assert not [r for r in sonos.requests[mark:] if not r[0].startswith('Get')], sonos.requests[mark:]
        writes = [r['request'] for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert writes == [mac + ' playlist index ' + str(index)], writes
        assert not [r for r in sonos.requests[mark:] if r[0] in ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
        wait(lambda: f'window pairing lms_index={index} decision=' in text()[logmark:],
             15, 'Q3 producer pairing completed before the next stimulus')
        app_latencies.append(stamp - start)
        print(f'PASS Q3 index={index} audible_seconds={stamp-start:.3f} commands=1 zero_edits=yes PCM=identical', flush=True)
    if os.environ.get('YENEY_FULLQ_PAUSED') == '1':
        sonos.transport_state = 'PAUSED_PLAYBACK'
        sonos.notify_transport()
        wait(lambda: json.loads(status_path.read_text()).get('playback', {}).get('state') == 'PAUSED', 10, 'paused speaker observed by bridge')
        mark = len(sonos.requests)
        paused_start = time.monotonic()
        cli('playlist index 2', mac)
        wait(lambda: sonos.transport_state == 'PLAYING' and any(r[0] >= paused_start and r[1] == immutable[2] for r in sonos.audible_reads), 15, 'paused move audible')
        paused_stamp = next(r[0] for r in sonos.audible_reads if r[0] >= paused_start and r[1] == immutable[2])
        calls = calls_until(mark, paused_stamp)
        report_calls('lms_paused_move', calls)
        assert [r[0] for r in calls if not r[0].startswith('Get')] == ['Seek', 'Play'], calls
        assert not [r for r in calls if r[0] == 'GetZoneGroupState'], calls
        print('PASS C1 paused move: one Seek TRACK_NR plus Play', flush=True)
    assert [i['uri'] for i in items()] == immutable
    spool = sum(p.stat().st_size for p in (c['base'] / 'queue').rglob('*') if p.is_file())
    quota = int(os.environ.get('YENEY_FULLQ_QUOTA_MIB', '64')) * 1024 * 1024
    assert spool <= quota, (spool, quota)
    status = Path(f'/proc/{c["bridge"].pid}/status').read_text()
    rss = next(line for line in status.splitlines() if line.startswith('VmRSS:'))
    print(f'MEASURE rows={count} spool_bytes={spool} quota_bytes={quota} {rss}', flush=True)
    if sonos.reject_batches:
        assert text().count('mirror batch rejected; AddURIToQueue fallback') == 1
        print('PASS Q7 batch rejection logged once; complete mirror preserved', flush=True)

    if os.environ.get('YENEY_FULLQ_QUOTA_MIB'):
        for index in range(12):
            logmark = len(text())
            cli('playlist index ' + str(index), mac)
            wait(lambda: len(items()) == count and sonos.position['TrackURI'] == items()[index]['uri'], 20, 'Q4 pressure move')
            u = urlsplit(items()[index]['uri'])
            conn = http.client.HTTPConnection(u.netloc, timeout=20)
            try:
                conn.request('GET', u.path)
                response = conn.getresponse()
                body = response.read()
                assert response.status == 200
                identity(index, body)
            finally:
                conn.close()
            wait(lambda: f'window pairing lms_index={index} decision=' in text()[logmark:], 15, 'Q4 paired pressure move')
        wait(lambda: 'cache evicted lms_index=0' in text(), 10, 'Q4 oldest played audio evicted')
        for app in (True, False):
            if not app:
                cli('playlist index 11', mac)
                wait(lambda: sonos.position['TrackURI'] == items()[11]['uri'], 20, 'Q4 LMS return')
            mark = len(text())
            if app: c['select_item'](0)
            else: cli('playlist index 0', mac)
            stamp, body = fetch(items()[0])
            identity(0, body)
            wait(lambda: int(cli('playlist index ?', mac)[-1]) == 0, 10, 'Q4 deep Back LMS follows')
            if app:
                wait(lambda: 'decision=re-produced reason=evicted' in text()[mark:], 10, 'Q4 reasoned eviction reproduction')
            assert [i['uri'] for i in items()] == immutable
        print('PASS Q4 deep app/LMS Back; evicted row reproduced, PCM identical, static URIs', flush=True)
    if os.environ.get('YENEY_FULLQ_GENERATION') == '1':
        old = set(immutable)
        logmark = len(text())
        start = time.monotonic()
        cli('playlistcontrol cmd:load track_id:' + ','.join(reversed(playlist)), mac)
        wait(lambda: sonos.position['TrackURI'] and sonos.position['TrackURI'] not in old, 90, 'Q6 new generation audible')
        wait(lambda: any(r[0] >= start and r[1] not in old for r in sonos.audible_reads), 30, 'Q6 new generation byte')
        stamp = next(r[0] for r in sonos.audible_reads if r[0] >= start and r[1] not in old)
        wait(lambda: len(items()) == count and all(i['uri'] not in old for i in items()), 300, 'Q6 complete new mirror')
        wait(lambda: 'window rebuild reason=playlist-change edits=' in text()[logmark:],
             10, 'Q6 confirmed generation model commit')
        assert [i['metadata']['title'] for i in items()] == [f'Title & {(count - 1 - n) % len(ids)}' for n in range(count)]
        pos = next(n for n, r in enumerate(sonos.audible_reads) if r[0] >= start and r[1] not in old)
        identity(count - 1, sonos.audible_bodies[pos])
        mark = len(sonos.requests)
        navigation_started = time.monotonic()
        cli('playlist index 1', mac)
        wait(lambda: sonos.position['TrackURI'] == items()[1]['uri'], 20, 'Q6 navigation after rebuild')
        wait(lambda: any(r[0] >= navigation_started and r[1] == items()[1]['uri'] for r in sonos.audible_reads), 15, 'Q6 navigation audible byte before teardown')
        assert not [r for r in sonos.requests[mark:] if r[0] in
                    ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
        print(f'PASS Q6 generation rebuilt; audible_seconds={stamp-start:.3f}', flush=True)

    print('MEASURE C2 ' + json.dumps(dict(codec=os.environ.get('YENEY_FULLQ_CODEC','flac'), rows=count, album=album_latency, lms=lms_latencies, app=app_latencies, build=build)), flush=True)
    assert max(lms_latencies) < 1.0, ('LMS audible latency target', lms_latencies)
    assert max(app_latencies) < .7, ('app audible latency target', app_latencies)
    assert album_latency < 1.5, ('album start latency target', album_latency)
