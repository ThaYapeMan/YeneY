# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Static playlist mirror acceptance against disposable LMS and renderer."""
import http.client
import json
import os
import time
from urllib.parse import urlsplit


def run(c):
    cli, mac, wait, items, sonos, proxy = (c[k] for k in ('cli', 'mac', 'wait', 'items', 'sonos', 'proxy'))
    ids = sorted(c['window_ids'], key=lambda i: int(next(t['url'].rsplit('/', 1)[-1].split('.')[0].rsplit('-', 1)[-1] for t in c['tracks'] if t['id'] == i)))
    count = int(os.environ.get('YENEY_FULLQ_ROWS', '12'))
    playlist = [ids[n % len(ids)] for n in range(count)]
    sonos.soap_delay = float(os.environ.get('YENEY_FULLQ_DELAY', '.6'))
    sonos.all_soap_delay = sonos.soap_delay if os.environ.get('YENEY_FULLQ_ALL_SOAP', '1') == '1' else 0
    sonos.fullq_audio = True
    sonos.reject_batches = os.environ.get('YENEY_FULLQ_REJECT_BATCHES') == '1'
    text = lambda: (c['base'] / 'bridge.log').read_text()
    def fetch(item):
        u = urlsplit(item['uri'])
        conn = http.client.HTTPConnection(u.netloc, timeout=10)
        try:
            conn.request('GET', u.path)
            reply = conn.getresponse()
            first = reply.read(1)
            stamp = time.monotonic()
            body = first + reply.read(44 + 4096 - 1)
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
        calls = sonos.requests[mark:]
        assert not [r for r in calls if r[0] in ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')], calls
        assert len([r for r in calls if r[0] == 'Seek' and r[1]['Unit'] == 'TRACK_NR']) == 1, calls
        wait(lambda: 'decision=paired' in text()[logmark:] or 'decision=re-produced' in text()[logmark:], 5, 'Q2 pairing decision')
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
        writes = [r['request'] for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert writes == [mac + ' playlist index ' + str(index)], writes
        assert not [r for r in sonos.requests[mark:] if r[0] in ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
        wait(lambda: f'window pairing lms_index={index} decision=' in text()[logmark:],
             15, 'Q3 producer pairing completed before the next stimulus')
        app_latencies.append(stamp - start)
        print(f'PASS Q3 index={index} audible_seconds={stamp-start:.3f} commands=1 zero_edits=yes PCM=identical', flush=True)
    assert [i['uri'] for i in items()] == immutable
    spool = sum(p.stat().st_size for p in (c['base'] / 'queue').rglob('*') if p.is_file())
    quota = int(os.environ.get('YENEY_FULLQ_QUOTA_MIB', '64')) * 1024 * 1024
    assert spool <= quota, (spool, quota)
    status = __import__('pathlib').Path(f'/proc/{c["bridge"].pid}/status').read_text()
    rss = next(line for line in status.splitlines() if line.startswith('VmRSS:'))
    print(f'MEASURE rows={count} spool_bytes={spool} quota_bytes={quota} {rss}', flush=True)
    if sonos.reject_batches:
        assert text().count('mirror batch rejected; AddURIToQueue fallback') == 1
        print('PASS Q7 batch rejection logged once; complete mirror preserved', flush=True)

    if os.environ.get('YENEY_FULLQ_QUOTA_MIB'):
        for index in range(12):
            logmark = len(text())
            cli('playlist index ' + str(index), mac)
            wait(lambda: sonos.position['TrackURI'] == items()[index]['uri'], 20, 'Q4 pressure move')
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
        assert 'window rebuild reason=playlist-change edits=' in text()[logmark:]
        assert [i['metadata']['title'] for i in items()] == [f'Title & {(count - 1 - n) % len(ids)}' for n in range(count)]
        pos = next(n for n, r in enumerate(sonos.audible_reads) if r[0] >= start and r[1] not in old)
        identity(count - 1, sonos.audible_bodies[pos])
        mark = len(sonos.requests)
        cli('playlist index 1', mac)
        wait(lambda: sonos.position['TrackURI'] == items()[1]['uri'], 20, 'Q6 navigation after rebuild')
        assert not [r for r in sonos.requests[mark:] if r[0] in
                    ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
        print(f'PASS Q6 generation rebuilt; audible_seconds={stamp-start:.3f}', flush=True)

    assert max(lms_latencies) < 1.5, ('LMS audible latency target', lms_latencies)
    assert max(app_latencies) < .7, ('app audible latency target', app_latencies)
    assert album_latency < 1.5, ('album start latency target', album_latency)
