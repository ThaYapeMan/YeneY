# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""LMS controller navigation against private LMS and the fake Sonos queue."""
import http.client
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
    # Real natural preparation and speaker boundaries fill both history slots.
    for index in (1, 2):
        for second in range(1, 24):
            time.sleep(1)
            with sonos.lock:
                sonos.position['RelTime'] = f'00:00:{second:02}'
        select_item(index)
        wait(lambda: int(cli('playlist index ?', mac)[-1]) == index, 5, 'natural LMS index')
        wait(lambda: len(items()) == index + 4, 5, 'natural history window')
    retained = {item['metadata']['title']: (item['uri'], fetch(item)) for item in items()[:3]}
    print('PASS real LMS navigation natural guard: two history parents, no ownership release', flush=True)

    def navigate(command, target, expected, history):
        before = len(proxy.records)
        log_start = len((base / 'bridge.log').read_text())
        removals = len(sonos.actions('RemoveAllTracksFromQueue'))
        starts = len(packets('STMs'))
        cli(command, mac)
        wait(lambda: 'window lms-nav ' in (base / 'bridge.log').read_text()[log_start:],
             15, 'navigation diagnostic')
        wait(lambda: titles() == expected, 5, 'navigation window')
        wait(lambda: len(packets('STMs')) > starts, 5, 'navigation audible start')
        time.sleep(.2)
        text = (base / 'bridge.log').read_text()[log_start:]
        assert f'to_index={target} action=slide history={history} lms_commands=0' in text, text
        assert 'window rebuild' not in text and 'ownership released' not in text, text
        assert 'queue LMS mutation ' not in text, text
        assert len(sonos.actions('RemoveAllTracksFromQueue')) == removals
        mutations = [r['request'] for r in proxy.records[before:]
                     if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert not mutations, mutations
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
    cli('stop', mac)
    wait(lambda: not items(), 10, 'final navigation release')
