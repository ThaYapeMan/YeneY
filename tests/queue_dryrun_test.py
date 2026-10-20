# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Real dry-run, real core, fake SlimProto and CLI; localhost only."""
import csv
import io
import http.server
import time
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'third_party/yeney-core/tests'))
from fake_lms import LMS, HTTP, SongQueue, listener


def wav(frames=24000):
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as out:
        out.setnchannels(2); out.setsampwidth(2); out.setframerate(8000)
        out.writeframes(struct.pack('<hh', 100, -100)*frames)
    return buf.getvalue()


class DryRunTests(unittest.TestCase):
    def test_headerless_flac_metadata(self):
        fixtures = ROOT/'third_party/yeney-core/tests/fixtures'
        full = (fixtures/'flac-16-2.flac').read_bytes()
        fixed = (fixtures/'queue-flac-seek-fixed.flac').read_bytes()
        variable = (fixtures/'queue-flac-seek-variable.flac').read_bytes()
        for mode in ('cache', 'http', 'fallback', 'no-id', 'other-track', 'resumed', 'late-flac'):
            with self.subTest(mode=mode):
                lms = LMS(listener(port=3483))
                cli = listener(port=9090)
                cli.settimeout(.1)
                done = threading.Event()
                errors, queries, requests = [], [], []
                model = None
                def serve_cli():
                    while not done.is_set():
                        try:
                            conn, _ = cli.accept()
                        except socket.timeout:
                            continue
                        try:
                            with conn:
                                conn.settimeout(1)
                                lines = conn.makefile('rb')
                                while True:
                                    query = lines.readline().decode().strip()
                                    if not query:
                                        break
                                    queries.append(query)
                                    self.assertTrue(query.startswith('02:59:51:54:00:01 '), query)
                                    key = query.split(' ', 1)[1]
                                    echo = '02%3A59%3A51%3A54%3A00%3A01 '
                                    if key.startswith('status '):
                                        index = key.split()[1]
                                        track_id = '41' if mode == 'other-track' and index == '0' else '42'
                                        value = f'{key} playlist_cur_index%3A0 playlist_index%3A{index}'
                                        if mode != 'no-id': value += f' id%3A{track_id} title%3ATest'
                                    else:
                                        index = max(0, model.playing-1) if model else 0
                                        if mode in ('resumed', 'late-flac'): index += 3
                                        value = key[:-1] + (str(index) if key.startswith('playlist index') else '0' if key.startswith('time') else 'Seek%20test')
                                    conn.sendall((echo+value+'\n').encode())
                        except Exception as error:
                            if not done.is_set(): errors.append(error)
                thread = threading.Thread(target=serve_cli)
                thread.start()
                class MetadataHandler(http.server.BaseHTTPRequestHandler):
                    def do_GET(self):
                        requests.append((self.path, self.headers.get('Range')))
                        if mode == 'fallback':
                            # Exceed the entire identity+metadata budget.
                            time.sleep(1.2)
                        self.send_response(206)
                        self.send_header('Content-Range', f'bytes 0-8191/{len(full)}')
                        self.send_header('Content-Length', '8192')
                        self.end_headers()
                        try:
                            self.wfile.write(full[:8192])
                        except (BrokenPipeError, ConnectionResetError):
                            pass
                    def log_message(self, *_):
                        pass
                metadata = http.server.ThreadingHTTPServer(('127.0.0.1', 9000), MetadataHandler)
                http_thread = threading.Thread(target=metadata.serve_forever)
                http_thread.start()
                bodies = [full, fixed, variable] if mode in ('cache', 'other-track') else [fixed, variable]
                if mode == 'late-flac': bodies.insert(0, wav())
                sources = [HTTP(body) for body in bodies]
                proc = subprocess.Popen([str(ROOT/'yeney-queue-dryrun'), '--server', '127.0.0.1', '--seconds', '4'],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    lms.accept()
                    model = SongQueue(lms, [(source, dict(fmt='p', rate=8000) if mode == 'late-flac' and i == 0 else dict(fmt='f')) for i, source in enumerate(sources)])
                    lms.wait('STMu', timeout=10)
                    stdout, stderr = proc.communicate(timeout=12)
                    self.assertEqual(proc.returncode, 0, stderr)
                    path = Path(next(line[8:] for line in stdout.splitlines() if line.startswith('Report: ')))
                    with path.with_suffix('.tsv').open() as data:
                        rows = list(csv.DictReader(data, delimiter='\t'))
                    self.assertEqual(len(rows), len(bodies))
                    for row in rows[1:] if mode in ('cache', 'other-track', 'late-flac') else rows:
                        self.assertEqual(int(row['actual_frames']), 1616)
                        if mode != 'late-flac':
                            self.assertLess(float(row['startup_delay_s']), 2)
                        self.assertEqual(row['publication'], 'sealed' if mode in ('fallback', 'no-id') else 'progressive')
                        self.assertEqual(row['exact_frames'], 'unknown' if mode in ('fallback', 'no-id') else '1616')
                    log = path.with_suffix('.log').read_text()
                    if mode == 'cache':
                        self.assertIn('FLAC STREAMINFO cached track_id=42', log)
                        self.assertIn('FLAC metadata cache track_id=42', log)
                        self.assertFalse(requests)
                    elif mode == 'no-id':
                        self.assertFalse(requests)
                        self.assertIn('CLI track id unavailable', log)
                    else:
                        self.assertEqual(requests[0], ('/music/42/download', 'bytes=0-8191'))
                        self.assertIn('metadata deadline' if mode == 'fallback' else 'FLAC metadata HTTP range', log)
                    if mode == 'late-flac':
                        self.assertTrue(any('status 4 1 tags:' in query for query in queries))
                        self.assertTrue(any('status 5 1 tags:' in query for query in queries))
                    if mode == 'resumed':
                        self.assertTrue(any('status 3 1 tags:' in query for query in queries))
                        self.assertTrue(any('status 4 1 tags:' in query for query in queries))
                    self.assertFalse(errors, errors)
                finally:
                    if proc.poll() is None:
                        proc.terminate(); proc.communicate(timeout=5)
                    done.set(); thread.join(timeout=2); cli.close(); lms.close()
                    metadata.shutdown(); metadata.server_close(); http_thread.join()
                    for source in sources: source.close()
        print('PASS headerless FLAC dry-run: fixed/variable exact remaining 1616; track-id cache, bounded metadata HTTP range and 1 s sealed fallback; audio only from strm')

    def test_progressive_start_and_cli(self):
        lms = LMS(listener(port=3483))
        cli = listener(port=9090)
        cli.settimeout(.1)
        done = threading.Event()
        queries, errors = [], []
        model = None
        def serve_cli():
            while not done.is_set():
                try:
                    conn, _ = cli.accept()
                except socket.timeout:
                    continue
                try:
                    with conn:
                        conn.settimeout(1)
                        lines = conn.makefile('rb')
                        for key in ('playlist index', 'time', 'title'):
                            query = lines.readline().decode().strip()
                            queries.append(query)
                            if query != f'02:59:51:54:00:01 {key} ?':
                                raise AssertionError(query)
                            index = max(0, model.playing-1) if model else 0
                            value = str(index) if key == 'playlist index' else '1.0' if key == 'time' else f'Track%20{index}'
                            conn.sendall(f'02%3A59%3A51%3A54%3A00%3A01 {key} {value}\n'.encode())
                except Exception as error:
                    errors.append(error)
        thread = threading.Thread(target=serve_cli)
        thread.start()
        sources = [HTTP(wav(128000)), HTTP(wav(128000))]
        proc = subprocess.Popen([str(ROOT/'yeney-queue-dryrun'), '--server', '127.0.0.1', '--seconds', '10', '--speed', '4'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            lms.accept()
            model = SongQueue(lms, [(source, dict(rate=8000)) for source in sources])
            lms.wait('STMu', timeout=10)
            stdout, stderr = proc.communicate(timeout=12)
            self.assertEqual(proc.returncode, 0, stderr)
            path = Path(next(line[8:] for line in stdout.splitlines() if line.startswith('Report: ')))
            with path.with_suffix('.tsv').open() as data:
                rows = list(csv.DictReader(data, delimiter='\t'))
            self.assertEqual(len(rows), 2)
            for row in rows:
                self.assertEqual(row['publication'], 'progressive')
                if row is rows[0]:
                    self.assertLess(float(row['startup_delay_s']), 2)
                self.assertLess(float(row['STMs']), float(row['STMd']))
                self.assertEqual(row['codec'], 'PCM')
                self.assertEqual(row['lms_matches_audible'], 'true')
                self.assertIn('Track ', row['lms_title'])
            self.assertEqual([p['playing_occurrence'] for p in lms.packets if p.get('event') == 'STMs'], [1, 2])
            self.assertFalse(any(p.get('event') == 'STMr' for p in lms.packets))
            self.assertGreaterEqual(len(queries), 15)
            self.assertFalse(errors, errors)
            self.assertIn('LMS mismatched seconds: 0', path.read_text())
            self.assertIn('Progress: completed_joins=1', stdout)
            self.assertIn('LMS read-ahead OK', path.read_text())
            self.assertAlmostEqual(float(rows[0]['audible_end']) / 4, float(rows[0]['audible_end_wall']), places=3)
            self.assertGreater(float(rows[0]['audible_end'])-float(rows[0]['audible_start']), 15)
            self.assertLess(float(rows[0]['audible_end_wall'])-float(rows[0]['audible_start_wall']), 4.2)
            self.assertTrue(any(p.get('event') == 'STMt' and p['elapsed'] > 10000 for p in lms.packets))
            print('PASS accelerated queue clock: 4x PCM audibility, simulated STMt elapsed, dual report clocks and live completed-join progress')
            print('PASS real dry-run: progressive audible startup <2 s, STMs before STMd, LMS matches both audible items; encoded LMS echo decoded; raw-MAC CLI at 1 Hz')
        finally:
            if proc.poll() is None:
                proc.terminate(); proc.communicate(timeout=5)
            done.set(); thread.join(timeout=2); cli.close(); lms.close()
            for source in sources:
                source.close()

    def test_local_flac_alac_startup_and_fact_e(self):
        fixture = ROOT/'third_party/yeney-core/tests/fixtures'
        for codec, name in [('f', 'flac-16-2.flac'), ('l', 'queue-alac-seek.m4a')]:
            with self.subTest(codec=codec):
                lms = LMS(listener(port=3483))
                source = HTTP((fixture/name).read_bytes())
                proc = subprocess.Popen([str(ROOT/'yeney-queue-dryrun'), '--server', '127.0.0.1', '--seconds', '3'],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    lms.accept()
                    SongQueue(lms, [(source, dict(fmt=codec))])
                    lms.wait('STMu', timeout=5)
                    stdout, stderr = proc.communicate(timeout=6)
                    self.assertEqual(proc.returncode, 0, stderr)
                    path = Path(next(line[8:] for line in stdout.splitlines() if line.startswith('Report: ')))
                    with path.with_suffix('.tsv').open() as data:
                        row = next(csv.DictReader(data, delimiter='\t'))
                    self.assertEqual(row['publication'], 'progressive')
                    self.assertLess(float(row['startup_delay_s']), 2)
                    self.assertLess(float(row['STMs']), 2)
                    self.assertEqual(row['codec'], 'FLAC' if codec == 'f' else 'ALAC')
                finally:
                    if proc.poll() is None:
                        proc.terminate(); proc.communicate(timeout=5)
                    lms.close(); source.close()
        print('PASS local FLAC/ALAC dry-run: exact progressive first item and STMs within 2 seconds, including rewritten seek header')
        lms = LMS(listener(port=3483))
        sources = [HTTP(struct.pack('<hh', 17, -17)*2400 + b'x'),
                   HTTP(wav(), first=16, delay=1), HTTP(wav(800))]
        proc = subprocess.Popen([str(ROOT/'yeney-queue-dryrun'), '--server', '127.0.0.1', '--seconds', '4'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            lms.accept()
            SongQueue(lms, [(source, dict(rate=8000)) for source in sources[:2]])
            lms.wait('DSCO'); lms.wait('STMs'); time.sleep(.4); lms.timer()
            self.assertFalse(any(p.get('event') == 'STMn' for p in lms.packets))
            lms.song_queue.advance()
            lms.wait('RESP')
            lms.song_queue = None
            lms.strm('f'); lms.wait('STMf')
            SongQueue(lms, [(sources[2], dict(rate=8000))], first=3)
            lms.wait('STMu', timeout=5)
            stdout, stderr = proc.communicate(timeout=6)
            self.assertEqual(proc.returncode, 0, stderr)
            self.assertEqual([p['playing_occurrence'] for p in lms.packets if p.get('event') == 'STMs'], [1,3])
            path = Path(next(line[8:] for line in stdout.splitlines() if line.startswith('Report: ')))
            with path.with_suffix('.tsv').open() as data:
                rows = list(csv.DictReader(data, delimiter='\t'))
            self.assertEqual([row['occurrence'] for row in rows], ['1','2','3'])
            self.assertEqual(rows[1]['STMs'], '-1')
            self.assertEqual(rows[1]['pair_status'], 'flushed')
            self.assertGreaterEqual(float(rows[2]['audible_start']), 0)
            print('PASS real bridge fact-E replay: reconnect source fault drains, successor flush, replacement audible; no premature STMs or skipped occurrence')
        finally:
            if proc.poll() is None:
                proc.terminate(); proc.communicate(timeout=5)
            lms.close()
            for source in sources:
                source.close()

    def test_empty_source_does_not_steal_successor(self):
        lms = LMS(listener(port=3483))
        stream = (ROOT/'third_party/yeney-core/tests/fixtures/queue-alac-seek.m4a').read_bytes()
        sources = [HTTP(stream[:stream.index(b'mdat') + 4]), HTTP(wav(800))]
        proc = subprocess.Popen([str(ROOT/'yeney-queue-dryrun'), '--server', '127.0.0.1', '--seconds', '3'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            lms.accept()
            SongQueue(lms, [(sources[0], dict(fmt='l')), (sources[1], dict(rate=8000))])
            lms.wait('DSCO');lms.timer()
            self.assertFalse(any(p.get('event')=='STMn' for p in lms.packets))
            lms.song_queue.advance() # host recovery, not StreamingFailed for received MP4 bytes
            lms.wait('STMu', timeout=5)
            stdout, stderr = proc.communicate(timeout=6)
            self.assertEqual(proc.returncode, 0, stderr)
            self.assertEqual([p['playing_occurrence'] for p in lms.packets if p.get('event') == 'STMs'], [2])
            path = Path(next(line[8:] for line in stdout.splitlines() if line.startswith('Report: ')))
            with path.with_suffix('.tsv').open() as data:
                rows = list(csv.DictReader(data, delimiter='\t'))
            self.assertEqual(rows[0]['STMs'], '-1')
            self.assertEqual(rows[0]['audible_start'], '-1')
            self.assertEqual(int(rows[0]['actual_frames']), 0)
            self.assertEqual(int(rows[1]['actual_frames']), 800)
            self.assertGreaterEqual(float(rows[1]['audible_start']), 0)
            print('PASS empty source bridge recovery: retire unplayed descriptor, no silence-only STMs, successor keeps its own occurrence')
        finally:
            if proc.poll() is None:
                proc.terminate(); proc.communicate(timeout=5)
            lms.close()
            for source in sources:
                source.close()

    def test_unknown_counts_seal_without_startup_deadlock(self):
        lms = LMS(listener(port=3483))
        sources = [HTTP(struct.pack('<hh', i, -i)*2400) for i in (17,23)]
        proc = subprocess.Popen([str(ROOT/'yeney-queue-dryrun'), '--server', '127.0.0.1', '--seconds', '3'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            lms.accept()
            SongQueue(lms, [(source, dict(rate=8000, transition=1, period=1)) for source in sources])
            lms.wait('STMu', timeout=5)
            stdout, stderr = proc.communicate(timeout=6)
            self.assertEqual(proc.returncode, 0, stderr)
            path = Path(next(line[8:] for line in stdout.splitlines() if line.startswith('Report: ')))
            with path.with_suffix('.tsv').open() as data:
                rows = list(csv.DictReader(data, delimiter='\t'))
            self.assertEqual([p['playing_occurrence'] for p in lms.packets if p.get('event') == 'STMs'], [1,2])
            self.assertEqual(sum(int(row['actual_frames']) for row in rows), 4800)
            self.assertTrue(all(row['publication'] == 'sealed' for row in rows), rows)
            self.assertTrue(all(row['transition'] == 'sealed-startup' for row in rows), rows)
            print('PASS unknown-count crossfade startup: sealed publication, explicit zero-bound fallback, all PCM preserved and no protocol deadlock')
        finally:
            if proc.poll() is None:
                proc.terminate(); proc.communicate(timeout=5)
            lms.close()
            for source in sources:
                source.close()

    def test_crossfade_deadline_preserves_all_pcm(self):
        lms = LMS(listener(port=3483))
        sources = [HTTP(wav(12000)), HTTP(wav(12000))]
        proc = subprocess.Popen([str(ROOT/'yeney-queue-dryrun'), '--server', '127.0.0.1', '--seconds', '5'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            lms.accept()
            model = SongQueue(lms, [(source, dict(rate=8000, transition=1, period=1)) for source in sources])
            lms.wait('STMu', timeout=8)
            stdout, stderr = proc.communicate(timeout=8)
            self.assertEqual(proc.returncode, 0, stderr)
            path = Path(next(line[8:] for line in stdout.splitlines() if line.startswith('Report: ')))
            with path.with_suffix('.tsv').open() as data:
                rows = list(csv.DictReader(data, delimiter='\t'))
            self.assertEqual([p['playing_occurrence'] for p in lms.packets if p.get('event') == 'STMs'], [1,2])
            self.assertEqual(sum(int(row['actual_frames']) for row in rows), 16000, rows)
            self.assertEqual(int(rows[0]['overlap_frames']), 8000, rows)
            self.assertTrue(all(float(row['audible_end']) >= 0 for row in rows), rows)
            self.assertTrue(any(row['transition'] == 'successor-deadline' for row in rows), rows)
            self.assertFalse(any(row['fault'] for row in rows), rows)
            print('PASS real crossfade dry-run: late final successor deadline, immutable promises and retained tail; all 16000 mixed frames played')
        finally:
            if proc.poll() is None:
                proc.terminate(); proc.communicate(timeout=5)
            lms.close()
            for source in sources:
                source.close()

if __name__ == '__main__':
    unittest.main(verbosity=2)
