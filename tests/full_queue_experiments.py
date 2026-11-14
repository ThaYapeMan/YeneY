# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Generated-file scaling and functional cache-pressure experiments; no C2 override."""
import http.client
import hashlib
import json
import os
import random
import re
import time
from urllib.parse import urlsplit


def run(c):
    assert os.environ['YENEY_FULLQ_EXPERIMENT'] in ('scale', 'pressure')
    cli, mac, wait, items, sonos, proxy, base = (c[k] for k in
        ('cli', 'mac', 'wait', 'items', 'sonos', 'proxy', 'base'))
    ids = sorted(c['window_ids'], key=lambda track_id: int(next(
        t['url'].rsplit('/',1)[-1].split('.')[0].rsplit('-',1)[-1]
        for t in c['tracks'] if t['id'] == track_id)))
    log = lambda: (base / 'bridge.log').read_text()
    sonos.latency_profile('device')
    sonos.fullq_audio = True
    def fetch(index, complete=False):
        uri = items()[index]['uri']
        u = urlsplit(uri)
        conn = http.client.HTTPConnection(u.netloc, timeout=120)
        try:
            conn.request('GET', u.path)
            response = conn.getresponse()
            body = response.read(45)
            stamp = time.monotonic()
            body += response.read() if complete else response.read(4095)
            assert response.status == 200 and body[:4] == b'RIFF'
            return stamp, body
        finally:
            conn.close()
    cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
    wait(lambda: len(items()) == len(ids) and bool(sonos.audible_reads), 90, 'experiment static mirror audible')
    immutable = [i['uri'] for i in items()]
    codec = os.environ.get('YENEY_FULLQ_CODEC', 'flac')
    if os.environ['YENEY_FULLQ_EXPERIMENT'] == 'scale':
        import numpy as np
        def expected_pcm(index):
            values = np.frombuffer((base/f'input-{index}.wav').read_bytes()[44:], dtype='<i2').astype('<i4')*256
            return values.view(np.uint8).reshape(-1,4)[:,:3].tobytes()
        # Request no unproduced row while preparing natural Next. HEAD cannot
        # activate; the full outgoing read and speaker clock allow decode-ahead.
        fetch(0, True)
        duration = ((base/'input-0.wav').stat().st_size-44)//(44100*4)
        with sonos.lock:
            seconds = duration - 15
            sonos.position['RelTime'] = f'{seconds//3600:02}:{seconds//60%60:02}:{seconds%60:02}'
        def prepared():
            u = urlsplit(immutable[1]); conn = http.client.HTTPConnection(u.netloc, timeout=3)
            try:
                conn.request('HEAD', u.path); r = conn.getresponse(); r.read()
                return r.status == 200
            finally: conn.close()
        wait(prepared, 120, 'decode-ahead row declared without lazy activation')
        _, reference = fetch(1, True)
        assert reference[44:] == expected_pcm(1), 'prepared Next frame-zero PCM'
        with sonos.lock:
            seconds = duration - 1
            sonos.position['RelTime'] = f'{seconds//3600:02}:{seconds//60%60:02}:{seconds%60:02}'
        wait(lambda: abs(float(cli('time ?', mac)[-1])-seconds) <= 1.25, 10, 'natural final-second clock')
        mark = len(proxy.records)
        start = time.monotonic(); c['select_item'](1); sonos.read_audio()
        natural = sonos.audible_reads[-1][0] - start
        assert sonos.audible_bodies[-1] == reference[:len(sonos.audible_bodies[-1])]
        assert not [r for r in proxy.records[mark:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        # Row 3 has never been requested or admitted by decode-ahead.
        u = urlsplit(immutable[3]); conn = http.client.HTTPConnection(u.netloc, timeout=3)
        conn.request('HEAD', u.path); r = conn.getresponse(); assert r.status != 200; r.read(); conn.close()
        mark, logmark = len(c['tap'].records), len(log())
        start = time.monotonic(); cli('playlist index 3', mac)
        wait(lambda: any(r[0] >= start and r[1] == immutable[3] for r in sonos.audible_reads), 120, 'unproduced jump first audible PCM')
        stamp = next(r[0] for r in sonos.audible_reads if r[0] >= start and r[1] == immutable[3])
        pos = next(n for n, r in enumerate(sonos.audible_reads) if r[0] >= start and r[1] == immutable[3])
        expected = expected_pcm(3)
        body = sonos.audible_bodies[pos]
        assert body[44:] == expected[:len(body)-44], 'unproduced jump frame-zero PCM'
        starts = [r for r in c['tap'].records[mark:] if r.get('command') == 's']
        selected_start = starts[-1]
        wire = chr(bytes.fromhex(selected_start['body'])[2])
        assert wire == {'alac': 'l', 'flac': 'f', 'wav': 'p'}[codec], ('source wire codec', codec, wire)
        track = next(t for t in c['tracks'] if t['id'] == ids[3])
        from urllib.parse import unquote
        from pathlib import Path
        source_path = Path(unquote(urlsplit(track['url']).path))
        size = source_path.stat().st_size
        atoms = {}
        if codec == 'alac':
            import struct
            data = source_path.read_bytes()
            offset = 0
            while offset+8 <= len(data):
                length, kind = struct.unpack_from('>I4s', data, offset)
                assert length >= 8
                atoms[kind.decode('ascii')] = offset
                offset += length
            assert (atoms['moov'] < atoms['mdat']) == (os.environ.get('YENEY_FULLQ_FASTSTART') == '1')
        decode = stamp-selected_start['when']
        boundaries = re.findall(r'core: boundary frame=\d+ rate=\d+ gapless=\d+ jiffies=(\d+)', log()[logmark:])
        assert boundaries
        production = (int(boundaries[-1]) - int(selected_start['when']*1000) % (2**32)) % (2**32) / 1000
        eof = [r['when'] for r in c['tap'].records[mark:] if r['op'] == 'DSCO' and selected_start['when'] <= r['when'] <= stamp]
        ingest = eof[-1]-selected_start['when'] if eof else None
        produced = re.findall(r'queue production occurrence=(\d+) content_frame=0', log()[logmark:])
        assert produced
        native_prefix = r'native input occurrence=' + produced[-1]
        native_first = re.search(native_prefix + r' first_pcm ingested_bytes=(\d+)', log()[logmark:])
        assert native_first, 'native input measurement missing'
        ingested_before_pcm = int(native_first[1])
        native_eof = re.search(native_prefix + r' encoded_eof bytes=(\d+) ingest_us=(\d+)', log()[logmark:])
        if native_eof:
            assert int(native_eof[1]) == size
            ingest = int(native_eof[2])/1000000
        result = dict(codec=codec, faststart=os.environ.get('YENEY_FULLQ_FASTSTART') == '1',
                      nominal_MB=c['scale_mb'], source_bytes=size, wire_codec=wire,
                      jump_seconds=stamp-start, strm_to_audible=decode,
                      first_production_seconds=production, eof_before_audible=bool(eof),
                      ingest_seconds=ingest, source_MB_per_second=size/1000000/ingest if ingest else None,
                      ingested_before_first_pcm=ingested_before_pcm,
                      natural_seconds=natural, atom_offsets=atoms)
        print('SCALING ' + json.dumps(result), flush=True)
        (base/'scaling.json').write_text(json.dumps(result, indent=2))
        return
    # Functional pressure uses full reference productions so converted audio is
    # compared byte for byte to its own from-start settled decode.
    reference = {}
    cli('playlist repeat 2', mac)
    for index in range(len(ids)):
        cli('playlist index ' + str(index), mac)
        wait(lambda: sonos.position['TrackURI'] == immutable[index], 20, 'pressure reference move')
        _, reference[index] = fetch(index, True)
        import numpy as np
        if codec == 'converted-flac':
            import subprocess
            conversion = subprocess.run([str(c['ROOT']/'third_party/yeney-core/mp4-position-test'),
                'convert', 'full'], input=(base/f'library/fullq-{index}.flac').read_bytes(), capture_output=True, check=True)
            decoded = conversion.stdout
            values = np.frombuffer(decoded[32:], dtype='<i4').astype(np.int64)
            values = np.where(values >= 0, (values+128)//256, -((-values+128)//256))
            values = np.clip(values, -(2**23), 2**23-1)
        else:
            values = np.frombuffer((base/f'input-{index}.wav').read_bytes()[44:], dtype='<i2').astype('<i4')*256
        expected = values.astype('<i4').view(np.uint8).reshape(-1,4)[:,:3].tobytes()
        assert len(reference[index]) == len(expected)+44
        if codec == 'converted-flac':
            # Standalone EOF pads the FIR tail; natural decode-ahead instead
            # supplies its successor there. Independently verify all earlier
            # samples, then compare complete Back WAVs to the natural reference.
            warmup = int(re.search(rb'warmup=(\d+)', conversion.stderr)[1])
            stop = max(4096*6, len(expected)-warmup*6)
            assert reference[index][44:44+stop] == expected[:stop], ('natural reference PCM', codec, index)
        else:
            assert reference[index][44:] == expected, ('natural reference PCM', codec, index)
    assert len({hashlib.sha256(body[44:]).digest() for body in reference.values()}) == len(ids)
    wait(lambda: 'cache evicted lms_index=0' in log(), 10, 'pressure evicts oldest audio')
    seed = int(os.environ.get('YENEY_FULLQ_SOAK_SEED', '0'))
    sequence = [(True, 0)] + [(False, index) for index in range(1, len(ids))] + [(False, 0)]
    if seed:
        rng = random.Random(seed)
        sequence += [(bool(rng.getrandbits(1)), rng.randrange(len(ids))) for _ in range(200)]
    current = len(ids)-1
    for step, (app, index) in enumerate(sequence):
        if index == current: index = (index+1) % len(ids)
        mark, soap, lm = len(log()), len(sonos.requests), len(proxy.records)
        move_start = time.monotonic()
        if app: c['select_item'](index)
        else:
            cli('playlist index ' + str(index), mac)
            wait(lambda: sonos.position['TrackURI'] == immutable[index], 20, 'pressure renderer target')
        _, body = fetch(index, step != len(ids)-1)
        assert body == reference[index][:len(body)], ('frame-zero PCM', codec, step, index)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == index, 10, 'pressure LMS follows')
        wait(lambda: f'window pairing lms_index={index} decision=' in log()[mark:], 15, 'pressure pairing')
        wait(lambda: sonos.position['TrackURI'] == immutable[index], 20, 'pressure renderer target')
        if not app:
            wait(lambda: any(r[0] >= move_start and r[1] == immutable[index] for r in sonos.audible_reads), 15, 'pressure target audible')
        assert [i['uri'] for i in items()] == immutable
        writes = [r['request'] for r in proxy.records[lm:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert len(writes) == int(app), writes
        seeks = [r for r in sonos.requests[soap:] if r[0] == 'Seek' and r[1].get('Unit') == 'TRACK_NR']
        assert len(seeks) == int(not app), seeks
        assert not [r for r in sonos.requests[soap:] if r[0] in ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
        print(f'PRESSURE seed={seed} step={step} app={app} index={index} PCM=identical rows={len(items())} commands={len(writes)} seeks={len(seeks)}', flush=True)
        current = index
    assert 'relay fault=' not in log() and 'lazy deadline' not in log()
    assert 'decision=re-produced reason=evicted' in log()
    wire_codecs = sorted({chr(bytes.fromhex(r['body'])[2]) for r in c['tap'].records if r.get('command') == 's'})
    assert wire_codecs == (['l'] if codec == 'alac' else ['f']), wire_codecs
    if codec == 'converted-flac':
        assert 'decimation source=96000 target=48000' in log()
    print(f'PASS D{3 if seed else 2} codec={codec} seed={seed} wire_codecs={wire_codecs} deep Back evicted PCM identical static URIs zero edits', flush=True)
