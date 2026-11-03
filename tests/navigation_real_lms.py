# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""LMS controller navigation against private LMS and the fake Sonos queue."""
import http.client
import re
import time
from urllib.parse import urlsplit


def run(context):
    cli, mac, wait, items, select_item, packets, sonos, proxy, base, ids = (
        context[k] for k in ('cli', 'mac', 'wait', 'items', 'select_item', 'packets',
                             'sonos', 'proxy', 'base', 'window_ids'))

    def titles():
        return [item['metadata']['title'] for item in items()]

    def fetch(item):
        url = urlsplit(item['uri'])
        conn = http.client.HTTPConnection(url.netloc, timeout=10)
        try:
            conn.request('GET', url.path)
            response = conn.getresponse()
            body = response.read()
            assert response.status == 200 and body[:4] == b'RIFF'
            return body
        finally:
            conn.close()

    cli('playlistcontrol cmd:load track_id:' + ids[3], mac)
    wait(lambda: len(items()) == 1, 15, 'reference target')
    select_item(0)
    reference = fetch(items()[0])
    cli('stop', mac)
    wait(lambda: not items(), 10, 'reference release')
    starts = len(packets('STMs'))
    cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
    wait(lambda: len(items()) == 4, 15, 'initial window')
    select_item(0)
    wait(lambda: len(packets('STMs')) > starts)
    # A direct click before decode-ahead has produced the target must still
    # publish the complete window, not just its new immutable current item.
    mark = len((base / 'bridge.log').read_text())
    before = len(proxy.records)
    cli('playlist index 3', mac)
    wait(lambda: titles() == [f'Title & {i}' for i in range(3, 7)], 15, 'direct click 0 to 3')
    time.sleep(1.5)
    text = (base / 'bridge.log').read_text()[mark:]
    assert text.count('window rebuild reason=out-of-range lms_commands=0') == 1, text
    assert 'to_index=3 action=rebuild history=0 lms_commands=0 kind=jump' in text, text
    assert not [r for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    print('PASS real LMS direct track click 0 to 3: current plus three upcoming, one rebuild, zero LMS commands', flush=True)
    from queue_sonos_fake import FiniteDeviceAudio
    model = FiniteDeviceAudio(sonos, progressive=True)
    sonos.device_audio = model
    model.start()
    wait(lambda: len(model.audio) >= 48000 * 6, 5, 'live current PCM')
    edits_before = len(sonos.edit_events)
    mark = len((base / 'bridge.log').read_text())
    uri = sonos.position['TrackURI']
    with sonos.lock:
        old = next(dict(fields) for _,fields in reversed(sonos.edit_events) if fields['CurrentTrackURI'] != uri)
        sonos.position_hook = lambda: sonos.notify_transport(old)
    wait(lambda: sonos.position_hook is None, 5, 'pure old edit echo')
    time.sleep(1)
    model.close();sonos.device_audio = None
    assert bytes(model.audio) == reference[44:44+len(model.audio)], 'live current PCM changed'
    assert len([r for r in model.requests if r[1] == uri]) == 1, model.requests
    assert len(sonos.edit_events) == edits_before
    assert 'window lms-nav ' not in (base / 'bridge.log').read_text()[mark:]
    assert sonos.position['TrackURI'] == uri
    print('PASS pure self-edit echo: no second edit/navigation, live playing PCM byte-identical, one GET without refetch', flush=True)
    cli('stop', mac)
    wait(lambda: not items(), 10, 'direct click release')
    starts = len(packets('STMs'))
    cli('playlistcontrol cmd:load track_id:' + ','.join(ids), mac)
    wait(lambda: len(items()) == 4, 15, 'restart natural window')
    select_item(0)
    wait(lambda: len(packets('STMs')) > starts)
    # Real natural preparation and speaker boundaries fill both history slots.
    for index in (1, 2):
        for second in range(1, 24):
            time.sleep(1)
            with sonos.lock:
                sonos.position['RelTime'] = f'00:00:{second:02}'
        mark = len((base / 'bridge.log').read_text())
        edit_start = len(sonos.edit_events)
        select_item(index)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == index, 5, 'natural LMS index')
        wait(lambda: len(items()) == index + 4, 5, 'natural history window')
        time.sleep(.8)
        text = (base / 'bridge.log').read_text()[mark:]
        assert text.count('window lms-nav ') == 1 and 'kind=natural' in text, text
        assert len(sonos.edit_events) - edit_start <= 2

    retained = {item['metadata']['title']: (item['uri'], fetch(item)) for item in items()[:3]}
    print('PASS real LMS navigation natural guard: two history parents, no ownership release', flush=True)

    def navigate(command, target, expected, history):
        before = len(proxy.records)
        log_start = len((base / 'bridge.log').read_text())
        removals = len(sonos.actions('RemoveAllTracksFromQueue'))
        edits_before = len(sonos.actions('AddURIToQueue')) + len(sonos.actions('RemoveTrackRangeFromQueue'))
        starts = len(packets('STMs'))
        cli(command, mac)
        wait(lambda: 'window lms-nav ' in (base / 'bridge.log').read_text()[log_start:],
             15, 'navigation diagnostic')
        wait(lambda: titles() == expected, 5, 'navigation window')
        # Force the production race: an old edit notification lands after the
        # position RPC captured its answer but before the worker receives it.
        with sonos.lock:
            old = next((dict(fields) for _,fields in reversed(sonos.edit_events)
                        if fields['CurrentTrackURI'] and fields['CurrentTrackURI'] != sonos.position['TrackURI']), None)
            if old:
                sonos.position_hook = lambda: sonos.notify_transport(old)
        if old:wait(lambda: sonos.position_hook is None, 5, 'delayed edit during position poll')
        wait(lambda: len(packets('STMs')) > starts, 5, 'navigation audible start')
        time.sleep(1.5) # drain delayed edit GENA and several ordinary polls
        edits = len(sonos.actions('AddURIToQueue')) + len(sonos.actions('RemoveTrackRangeFromQueue')) - edits_before
        bound = 4 if command.endswith(('+1', '-1')) else 2 if command.startswith('time ') else 12
        assert edits <= bound, ('unbounded navigation edits', edits, bound)
        stable = len(sonos.requests)
        time.sleep(.6)
        assert not [r for r in sonos.requests[stable:] if r[0] in ('AddURIToQueue','RemoveTrackRangeFromQueue')]

        text = (base / 'bridge.log').read_text()[log_start:]
        assert f'to_index={target} action=slide history={history} lms_commands=0' in text, text
        kind = 'seek' if command.startswith('time ') else 'next' if command.endswith('+1') else 'previous' if command.endswith('-1') else 'jump'
        assert 'kind='+kind in text, text
        assert text.count('window lms-nav ') == 1, text
        audible = re.findall(r'queue state=PLAYING audible occurrence=(\d+)', text)
        assert len(set(audible)) <= 1, ('audible oscillation', audible)
        assert 'window rebuild' not in text and 'ownership released' not in text, text
        assert 'queue LMS mutation ' not in text, text
        assert len(sonos.actions('RemoveAllTracksFromQueue')) == removals
        mutations = [r['request'] for r in proxy.records[before:]
                     if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert not mutations, mutations
        print(f'PASS echo convergence kind={kind} edits={edits} bound={bound} stable_polls=yes no_occurrence_oscillation=yes', flush=True)
        return text

    navigate('playlist index +1', 3, [f'Title & {i}' for i in range(1, 7)], 2)
    assert fetch(items()[2]) == reference, 'user Next PCM differs from natural target'
    for item in items()[:2]:
        uri, body = retained[item['metadata']['title']]
        assert item['uri'] == uri and fetch(item) == body
    print('PASS real LMS user Next mid-track: surgical slide, two retained history parents, byte-identical audio, zero LMS commands, no full flush', flush=True)
    text = navigate('playlist index -1', 2, [f'Title & {i}' for i in range(1, 6)], 1)
    assert 'retained target audio reused' in text, text
    current = items()[1]
    assert current['uri'] == retained['Title & 2'][0] and fetch(current) == retained['Title & 2'][1]
    print('PASS real LMS user Previous mid-track: slide, correct retained history/upcoming, target audio reused, zero LMS commands', flush=True)
    next_uri = items()[2]['uri']
    starts = len(packets('STMs'))
    before = len(proxy.records)
    for second in range(1, 24):
        time.sleep(1)
        with sonos.lock:
            sonos.position['RelTime'] = f'00:00:{second:02}'
    select_item(2)
    wait(lambda: len(packets('STMs')) > starts and int(cli('playlist index ?', mac)[-1]) == 3,
         5, 'natural advance after Previous')
    wait(lambda: titles() == [f'Title & {i}' for i in range(1, 7)], 5, 'post-Previous natural window')
    assert items()[2]['uri'] == next_uri
    assert not [r for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    print('PASS real LMS natural advance after Previous: retained upcoming URI reused with fresh producer clock, no duplicate parent or LMS command', flush=True)
    navigate('time 12', 3, [f'Title & {i}' for i in range(1, 7)], 2)
    print('PASS real LMS seek current item: surgical replacement, no window rebuild, zero LMS commands', flush=True)
    navigate('playlist index 0', 0, [f'Title & {i}' for i in range(4)], 0)
    print('PASS real LMS nearby jump: absent target bridges through retained neighbours; seek suffix replaced by full upcoming declaration; zero LMS commands', flush=True)
    before = len(proxy.records)
    log_start = len((base / 'bridge.log').read_text())
    cli('playlist index 7', mac)
    wait(lambda: 'to_index=7 action=rebuild' in (base / 'bridge.log').read_text()[log_start:], 15, 'far navigation')
    text = (base / 'bridge.log').read_text()[log_start:]
    assert text.count('window rebuild reason=out-of-range lms_commands=0') == 1, text
    assert 'queue LMS mutation ' not in text, text
    assert titles() == ['Title & 7'], titles()
    assert not [r for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    print('PASS real LMS far jump: one out-of-range rebuild, no fabricated history, zero LMS commands', flush=True)
    # No retained overlap, with three following tracks still in the playlist.
    before = len(proxy.records)
    log_start = len((base / 'bridge.log').read_text())
    edits_before = len(sonos.actions('AddURIToQueue')) + len(sonos.actions('RemoveTrackRangeFromQueue'))
    cli('playlist index 3', mac)
    wait(lambda: titles() == [f'Title & {i}' for i in range(3, 7)], 15, 'full no-overlap jump window')
    time.sleep(1.5)
    text = (base / 'bridge.log').read_text()[log_start:]
    assert text.count('window rebuild reason=out-of-range lms_commands=0') == 1, text
    assert text.count('window lms-nav ') == 1 and 'kind=jump' in text, text
    edits = len(sonos.actions('AddURIToQueue')) + len(sonos.actions('RemoveTrackRangeFromQueue')) - edits_before
    assert edits <= 12, edits
    assert not [r for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    assert sonos.edit_events and 'self-edit echo fenced' in (base / 'bridge.log').read_text()
    print(f'PASS real LMS direct non-adjacent jump: one rebuild, current plus three upcoming, zero LMS commands; echo edits={edits} bound=12', flush=True)
    before = len(proxy.records)
    target = items()[1]
    select_item(1) # Sonos app Next: play the unbound next row and fetch its PCM.
    body = fetch(target)
    wait(lambda: int(cli('playlist index ?', mac)[-1]) == 4, 5, 'app Next LMS index')
    time.sleep(1)
    mutations = [r['request'] for r in proxy.records[before:]
                 if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    assert mutations == [mac + ' playlist index 4'], mutations
    assert body[:4] == b'RIFF'
    print('PASS real LMS app Next guard: echoing speaker, lazy activation, exactly one playlist index command', flush=True)
    cli('stop', mac)
    wait(lambda: not items(), 10, 'final navigation release')
