# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Replace a playing LMS album through the same CLI command as a row's Play."""
from bisect import bisect_left
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from http.client import HTTPConnection
from threading import Event, Thread
from urllib.parse import urlsplit
import time


def read_prefix(uri, opened):
    url = urlsplit(uri)
    conn = HTTPConnection(url.netloc, timeout=8)
    try:
        conn.request('GET', url.path)
        response = conn.getresponse()
        assert response.status == 200
        opened.set()
        assert response.read(44).startswith((b'RIFF', b'fLaC'))
    finally:
        conn.close()


def run(c, case):
    cli, wait, mac, items, sonos, proxy, ids, base = (
        c[key] for key in ('cli', 'wait', 'mac', 'items', 'sonos', 'proxy', 'window_ids', 'base'))
    album = list(range(19))
    title = lambda i: f'Title & {i}'
    log_path = base / 'bridge.log'

    def load(indices):
        cli('playlistcontrol cmd:load track_id:' + ','.join(ids[i] for i in indices), mac)

    def playing(indices):
        rows = items()
        return ([row['metadata']['title'] for row in rows] == [title(i) for i in indices]
                and sonos.transport_state == 'PLAYING' and sonos.position['TrackURI'] == rows[0]['uri'])

    cli('playlist repeat 0', mac)
    cli('playlist shuffle 0', mac)
    load(album)
    wait(lambda: playing(album), 15, '19-row album playing')
    with ThreadPoolExecutor(max_workers=1) as fetcher:
        prefix = Event()
        read_prefix(items()[0]['uri'], prefix)
        if case == 'ahead':
            fetcher.submit(read_prefix, items()[1]['uri'], prefix := Event())
            wait(prefix.is_set, 5, 'speaker fetching ahead')
        elif case == 'next':
            c['select_item'](1)
            wait(lambda: cli('playlist index ?', mac)[-1] == '1', 2, 'Next reaches LMS')
        elif case in ('3', '8', '60', 'replay'):
            time.sleep(7 if case == 'replay' else int(case))

        wanted = {'four': [4, 5, 6, 7], 'equal': album, 'disjoint': [19]}.get(case, [4])
        read_mark, log_mark = len(proxy.records), log_path.stat().st_size
        with sonos.lock:
            soap_mark = len(sonos.requests)
        if case == 'clear':
            cli('playlist clear', mac)
            wait(lambda: not items() and sonos.transport_state == 'STOPPED', 8, 'busy clear')
            assert not any(x.startswith('id:') for x in cli('status 0 100 tags:u', mac))
            print('PASS playqueue clear: LMS and speaker stopped with empty queues', flush=True)
            return

        timings = defaultdict(list)
        repeats = defaultdict(list)
        ready = Event()

        def observe_log():
            with log_path.open() as stream:
                stream.seek(log_mark)
                ready.set()
                while not finished.is_set():
                    line = stream.readline()
                    if not line:
                        finished.wait(.002)
                        continue
                    now = time.monotonic()
                    if 'core: queue command strm s' in line:
                        timings['stream'].append(now)
                    if 'queue SOAP action=Play ' in line:
                        timings['play'].append(now)
                    repeats[line.strip()].append(now)

        finished = Event()
        monitor = Thread(target=observe_log)
        monitor.start()
        ready.wait()
        try:
            load(wanted)
            wait(lambda: playing(wanted) and timings['stream'] and timings['play'],
                 89 if case == 'replay' else 5, f'{case} replacement plays with exact LMS rows')
            read_prefix(items()[0]['uri'], Event())
            status = cli('status 0 100 tags:u', mac)
            assert 'playlist_cur_index:0' in status, status
            assert [entry[3:] for entry in status if entry.startswith('id:')] == [ids[i] for i in wanted], status
            delay = timings['play'][0] - timings['stream'][0]
            assert 0 <= delay <= 1, delay
            writes = Counter(action for action, *_ in sonos.requests[soap_mark:])
            print(f'playqueue {case}: stream-to-Play={delay:.3f}s Stop={writes["Stop"]} '
                  f'RemoveTrackRangeFromQueue={writes["RemoveTrackRangeFromQueue"]}', flush=True)
            assert writes['Stop'] <= 1 and writes['RemoveTrackRangeFromQueue'] <= 1, writes
            print(f'PASS playqueue {case}: expected LMS index and exact speaker rows', flush=True)
        finally:
            finished.set()
            monitor.join()
            reads = [r['when'] for r in proxy.records[read_mark:] if ' status ' in r['request']]
            def peak(stamps):
                return max((bisect_left(stamps, stamp + 1) - i
                            for i, stamp in enumerate(stamps)), default=0)
            status_peak = peak(reads)
            log_peak = max(map(peak, repeats.values()), default=0)
            print(f'playqueue {case}: status/s={status_peak} repeated-line/s={log_peak}', flush=True)
        assert status_peak <= 4 and log_peak <= 4
