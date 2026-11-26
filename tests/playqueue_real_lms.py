# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Replace a playing LMS album through the same CLI command as a row's Play."""
from bisect import bisect_left
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from http.client import HTTPConnection
import shutil
import sys
from threading import Event, Thread
from urllib.parse import unquote, urlsplit
import time


def instrument_lms(source, directory):
    """Make the load's incomplete list visible to real LMS CLI requests."""
    commands = directory / 'Slim/Control'
    commands.mkdir(parents=True)
    for relative, excluded in (('.', {'Slim', 'slimserver.pl'}), ('Slim', {'Control'}),
                               ('Slim/Control', {'Commands.pm'})):
        for entry in (source / relative).iterdir():
            if entry.name not in excluded:
                (directory / relative / entry.name).symlink_to(entry.resolve())
    shutil.copyfile(source / 'slimserver.pl', directory / 'slimserver.pl')
    original = (source / 'Slim/Control/Commands.pm').read_text()
    anchor = '$size = Slim::Player::Playlist::addTracks($client, \\@tracks, $insert);'
    assert original.count(anchor) == 1
    staged = '''if ($load && @tracks == 4) {
            my @first = splice(@tracks, 0, 1);
            Slim::Player::Playlist::addTracks($client, \\@first, 0);
            Slim::Player::Playlist::reshuffle($client, 1);
            my $snapshot = Slim::Control::Request->new($client->id(),
                ['status', '0', '60000', 'tags:alKudVT'], 1);
            $snapshot->execute();
            my @fields = $snapshot->renderAsArray();
            my $wire = Slim::Control::Stdio::array_to_string($snapshot->clientid(), \\@fields);
            open(my $out, '>', $ENV{YENEY_PLAYQUEUE_STAGE} . '.tmp') or die $!;
            print {$out} $wire . "\\n";
            close($out) or die $!;
            rename($ENV{YENEY_PLAYQUEUE_STAGE} . '.tmp', $ENV{YENEY_PLAYQUEUE_STAGE}) or die $!;
        }
        '''
    (commands / 'Commands.pm').write_text(original.replace(anchor, staged + anchor))
    return directory


def read_prefix(uri, opened, hold=None):
    url = urlsplit(uri)
    conn = HTTPConnection(url.netloc, timeout=8)
    try:
        conn.request('GET', url.path)
        response = conn.getresponse()
        assert response.status == 200
        opened.set()
        assert response.read(44).startswith((b'RIFF', b'fLaC'))
        if hold is not None:
            assert hold.wait(8), 'ahead request was never released'
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
        ahead = hold = None
        if case == 'ahead':
            prefix, hold = Event(), Event()
            ahead = fetcher.submit(read_prefix, items()[1]['uri'], prefix, hold)
            wait(prefix.is_set, 5, 'speaker fetching ahead')
        elif case == 'next':
            c['select_item'](1)
            wait(lambda: cli('playlist index ?', mac)[-1] == '1', 2, 'Next reaches LMS')
        elif case in ('3', '8', '60', 'replay'):
            time.sleep(7 if case == 'replay' else int(case))

        wanted = {'four': [4, 5, 6, 7], 'half-built': [4, 5, 6, 7],
                  'equal': album, 'disjoint': [19]}.get(case, [4])
        if case in ('half-built', 'unconfirmed'):
            staged = base / 'half-built.status'

            def during_load(request):
                if b' status 0 60000 ' not in request:
                    return None
                try:
                    return staged.read_bytes()
                except FileNotFoundError:
                    return None
                finally:
                    if case == 'half-built':
                        staged.unlink(missing_ok=True)

            proxy.request_hook = during_load
        read_mark, log_mark = len(proxy.records), log_path.stat().st_size
        with sonos.lock:
            soap_mark = len(sonos.requests)
        timings = defaultdict(list)
        repeats = defaultdict(list)
        ready = Event()
        action = 'Seek' if case == 'equal' else 'Play'

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
                    if f'queue SOAP action={action} ' in line:
                        timings['switch'].append(now)
                    if line.startswith(('yeney:', 'core:')):
                        repeats[line.strip()].append(now)

        finished = Event()
        monitor = Thread(target=observe_log)
        monitor.start()
        ready.wait()
        try:
            if case == 'clear':
                cli('playlist clear', mac)
                wait(lambda: not items() and sonos.transport_state == 'STOPPED', 8, 'busy clear')
                assert not any(x.startswith('id:') for x in cli('status 0 100 tags:u', mac))
                time.sleep(2)
                recent = log_path.read_text()[log_mark:]
                assert recent.count('rebuild reason=clear') == 1, recent
                print('PASS playqueue clear: LMS and speaker stopped with empty queues', flush=True)
                return
            if case == 'unconfirmed':
                load([4, 5, 6, 7])
                wait(lambda: 'decision=retain-last-confirmed reason=retry-expired' in log_path.read_text(),
                     3, 'unconfirmed short snapshot expires')
                reads_at_expiry = len(proxy.records)
                time.sleep(.55)
                assert len(proxy.records) == reads_at_expiry, 'expired admission still retries'
                assert [row['metadata']['title'] for row in items()] == [title(i) for i in album]
                assert 'queue SOAP action=Stop ' not in log_path.read_text()[log_mark:]
                proxy.request_hook = None
                timings.clear()
                print('PASS playqueue unconfirmed: bounded refusal retains all 19 rows', flush=True)
            load(wanted)
            if hold is not None:
                hold.set()
                ahead.result()
            wait(lambda: playing(wanted) and timings['stream'] and timings['switch'],
                 89 if case == 'replay' else 5, f'{case} replacement plays with exact LMS rows')
            read_prefix(items()[0]['uri'], Event())
            status = cli('status 0 100 tags:u', mac)
            assert 'playlist_cur_index:0' in status, status
            assert [entry[3:] for entry in status if entry.startswith('id:')] == [ids[i] for i in wanted], status
            delay = timings['switch'][0] - timings['stream'][0]
            assert 0 <= delay <= 1, delay
            writes = Counter(action for action, *_ in sonos.requests[soap_mark:])
            print(f'playqueue {case}: stream-to-{action}={delay:.3f}s Stop={writes["Stop"]} '
                  f'RemoveTrackRangeFromQueue={writes["RemoveTrackRangeFromQueue"]}', flush=True)
            replacements = 0 if case == 'equal' else 1
            assert writes['Stop'] == writes['RemoveTrackRangeFromQueue'] == replacements, writes
            if case == 'half-built':
                snapshots = [unquote(record['reply']).split() for record in proxy.records[read_mark:]
                             if ' status ' in record['request']]
                intermediate = [snapshot for snapshot in snapshots if 'playlist_tracks:1' in snapshot]
                counts = [field for snapshot in snapshots for field in snapshot if field.startswith('playlist_tracks:')]
                print(f'playqueue half-built: bridge CLI counts={counts}', flush=True)
                assert intermediate, 'bridge did not read the actual half-built LMS list'
                print(f'playqueue half-built: real CLI responses with a one-row prefix={len(intermediate)}',
                      flush=True)
            print(f'PASS playqueue {case}: expected LMS index and exact speaker rows', flush=True)
        finally:
            if hold is not None:
                hold.set()
            finished.set()
            monitor.join()
            reads = [r['when'] for r in proxy.records[read_mark:] if ' status ' in r['request']]
            def peak(stamps):
                return max((bisect_left(stamps, stamp + 1) - i
                            for i, stamp in enumerate(stamps)), default=0)
            status_peak = peak(reads)
            log_peak = max(map(peak, repeats.values()), default=0)
            print(f'playqueue {case}: status/s={status_peak} repeated-line/s={log_peak}', flush=True)
            for line, stamps in repeats.items():
                if peak(stamps) > 4:
                    print(f'playqueue {case}: {peak(stamps)}/s {line}', flush=True)
            if sys.exc_info()[0] is None:
                assert status_peak <= 4 and log_peak <= 4
