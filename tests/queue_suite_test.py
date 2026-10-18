# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Unattended queue suite against localhost CLI and SlimProto, never real LMS."""
import csv
import io
import os
from pathlib import Path
import signal
import socket
import socketserver
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
from urllib.parse import quote, unquote

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
from queue_suite_runner import suite
sys.path.insert(0, str(ROOT/'third_party/yeney-core/tests'))
from fake_lms import LMS, HTTP, SongQueue, listener
from decoders_test import alac_parts, mp4

MAC = suite.MAC


def library():
    tracks = []
    for kind, base in [('alc', 100), ('flc', 200)]:
        for album in range(4):
            for pos in range(4 if album == 0 else 1):
                ident = str(base+album*10+pos)
                tracks.append(dict(id=ident, title=f'Track {ident}: + café', album_id=str(base+album),
                    album=f'Album {base+album}', duration='60.04', samplerate='44100', type=kind,
                    url=f'file:///music/{ident}', remote='0', tracknum=str(pos+1)))
    return tracks


def crc(data, width, poly):
    value = 0
    for byte in data:
        value ^= byte << (width-8)
        for _ in range(8):
            value = ((value << 1) ^ (poly if value & (1 << (width-1)) else 0)) & ((1 << width)-1)
    return value


def flac_silence(blocks=646):
    # Original constant-subframe encoder, RFC 9639; no encoder dependency.
    count = blocks*4096
    info = b'\x10\x00\x10\x00'+bytes(6)
    info += ((44100 << 44) | (1 << 41) | (15 << 36) | count).to_bytes(8, 'big')+bytes(16)
    data = bytearray(b'fLaC\x80\x00\x00\x22'+info)
    for number in range(blocks):
        encoded = bytes([number]) if number < 128 else bytes([0xc0 | (number >> 6), 0x80 | (number & 63)])
        header = b'\xff\xf8\xc9\x18'+encoded
        frame = header+bytes([crc(header, 8, 7)])+bytes(6)
        data.extend(frame+crc(frame, 16, 0x8005).to_bytes(2, 'big'))
    return bytes(data)


class PatientHTTP(HTTP):
    def run(self):
        self.socket.settimeout(90)
        super().run()


class FakeLMS:
    def __init__(self, real=False, fail_load=False, grouped=False):
        self.real, self.fail_load, self.grouped = real, fail_load, grouped
        self.commands, self.errors, self.sources = [], [], []
        self.prefs = dict(transitionType='3', transitionDuration='7', transitionSmart='1', shuffle='2', repeat='1')
        self.original = self.prefs.copy()
        self.tracks = library()
        self.playlist = []
        self.lms = self.model = None
        self.connected = False
        self.session_count = self.auto_resume_count = 0
        self.done = threading.Event()
        self.lock = threading.RLock()
        self.played = threading.Event()
        if real:
            cookie, packets = alac_parts((ROOT/'third_party/yeney-core/tests/fixtures/alac-16-2.m4a').read_bytes())
            self.alac = mp4(cookie, [packets[0]]*646, 646*4096)
            self.flac = flac_silence()
        else:
            self.alac = self.flac = b''
        owner = self
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                for raw in self.rfile:
                    request = [unquote(word) for word in raw.decode().strip().split()]
                    try:
                        answer = owner.command(request)
                        line = ' '.join(quote(str(w), safe='') for w in answer)+'\n'
                        self.wfile.write(line.encode())
                    except Exception as error:
                        owner.errors.append(error)
                        return
        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True
        self.cli = Server(('127.0.0.1', 9090), Handler)
        self.cli_thread = threading.Thread(target=self.cli.serve_forever)
        self.cli_thread.start()
        self.slim = listener(port=3483)
        self.slim.settimeout(.1)
        self.slim_thread = threading.Thread(target=self.serve_slim)
        self.slim_thread.start()

    def source(self, data):
        source = PatientHTTP(data)
        self.sources.append(source)
        return source

    def serve_slim(self):
        owner = self
        class LockedLMS(LMS):
            def send(self, *args):
                with owner.lock:
                    return super().send(*args)
        while not self.done.is_set():
            if not self.real:
                try:
                    conn, _ = self.slim.accept()
                except socket.timeout:
                    continue
                self.connected = True
                self.session_count += 1
                self.auto_resume_count += 1
                try:
                    while conn.recv(1024):
                        pass
                finally:
                    self.connected = False
                    conn.close()
                continue
            lms = LockedLMS(self.slim)
            try:
                lms.accept()
            except socket.timeout:
                continue
            try:
                with self.lock:
                    self.lms, self.model = lms, None
                    self.connected = True
                    self.session_count += 1
                    # Reconnect automatically streams the old playlist before owner actions.
                    lms.strm('s', self.source(self.alac), fmt='l')
                    self.auto_resume_count += 1
                while not self.done.is_set():
                    if lms.read(eof_ok=True) is None:
                        break
            except (OSError, AssertionError) as error:
                if not self.done.is_set() and 'disconnect' not in str(error):
                    self.errors.append(error)
            finally:
                self.connected = False
                if lms.connection:
                    lms.connection.close()

    def command(self, words):
        with self.lock:
            self.commands.append(words)
            head = words[0]
            if head == 'titles':
                start, size = map(int, words[1:3])
                # Exercise pagination with a server that returns smaller pages.
                size = min(size, 5)
                page = self.tracks[start:start+size]
                return words+['count:'+str(len(self.tracks))]+[f'{k}:{v}' for t in page for k, v in t.items()]
            if head == 'syncgroups':
                members = MAC+',aa:bb:cc:dd:ee:ff' if self.grouped else 'aa:bb:cc:dd:ee:ff,bb:cc:dd:ee:ff:00'
                return words[:-1]+['sync_members:'+members]
            if head == 'players':
                return words+['count:2', 'playerindex:0', 'playerid:aa:bb:cc:dd:ee:ff', 'connected:1',
                    'playerindex:1', 'playerid:'+MAC, 'name:Queue Test (YeneY)', 'connected:'+str(int(self.connected))]
            if head != MAC:
                raise AssertionError('touched another player: '+repr(words))
            op = words[1:]
            if op[0] == 'playerpref':
                key, value = op[1:]
                if value == '?':
                    return words[:-1]+[self.prefs[key]]
                self.prefs[key] = value
            elif op in (['stop'], ['playlist', 'clear']):
                if self.real and self.connected:
                    self.lms.song_queue = None
                    self.model = None
                    self.lms.strm('q')
                if op == ['playlist', 'clear']:
                    self.playlist = []
            elif op[0] == 'playlistcontrol':
                if self.fail_load:
                    return words+['error:injected-load-failure']
                ids = next(w[9:] for w in op if w.startswith('track_id:')).split(',')
                self.playlist = ids
                return words+['count:'+str(len(ids))]
            elif op == ['play']:
                self.played.set()
                if self.real:
                    by_id = {t['id']: t for t in self.tracks}
                    sources = []
                    for ident in self.playlist:
                        kind = by_id[ident]['type']
                        body, fmt = (self.alac, 'l') if kind == 'alc' else (self.flac, 'f')
                        sources.append((self.source(body), dict(fmt=fmt, transition=int(self.prefs['transitionType']), period=int(self.prefs['transitionDuration']))))
                    self.model = SongQueue(self.lms, sources)
            elif op[0] == 'status':
                index = int(op[1])
                return words+['playlist_index:'+str(index), 'id:'+self.playlist[index]]
            elif op[-1] == '?':
                index = max(0, self.model.playing-1) if self.model else 0
                if op[:-1] == ['playlist', 'index']:
                    value = str(index)
                elif op[:-1] == ['time']:
                    packets = [p for p in (self.lms.packets if self.lms else []) if p.get('event') in ('STMs', 'STMt')]
                    value = str(packets[-1]['elapsed']/1000) if packets else '0'
                else:
                    value = 'Track + café'
                return words[:-1]+[value]
            else:
                raise AssertionError('unexpected command '+repr(words))
            return words

    def close(self):
        self.done.set()
        if self.lms and self.lms.connection:
            try:
                self.lms.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.slim_thread.join(timeout=6)
        self.slim.close()
        self.cli.shutdown(); self.cli.server_close(); self.cli_thread.join()
        for source in self.sources:
            source.close()


FAKE_DRYRUN = r'''#!/usr/bin/env python3
import csv, os, signal, socket, sys, time
from pathlib import Path
mode = Path(__file__).stem
if mode == 'busy':
    print('Another dry-run owns the fixed test player', flush=True)
    sys.exit(1)
conn = socket.create_connection(('127.0.0.1', 3483))
base = Path(__file__).parent/('report-'+str(os.getpid()))
print('Report: '+str(base)+'.md', flush=True)
done = False
def stop(*_):
    global done
    done = True
signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGUSR1, lambda *_: print('Measurement ready', flush=True))
start = time.monotonic()
while not done:
    time.sleep(.02)
    if mode not in ('hang', 'error') and time.monotonic()-start > .4:
        print('Progress: completed_joins=3', flush=True)
        time.sleep(.2)
    if mode == 'error' and time.monotonic()-start > .4:
        sys.exit(7)
with base.with_suffix('.tsv').open('w') as out:
    fields = ['pair_status','audible_start','fault','codec','rate','overlap_frames','startup_delay_s','successor_lead_s','lms_matches_audible']
    w = csv.DictWriter(out, fields, delimiter='\t'); w.writeheader()
    for i in range(3):
        w.writerow(dict(pair_status='completed',audible_start=1,fault='',codec='ALAC',rate=44100,overlap_frames=441000,startup_delay_s=.5,successor_lead_s=59,lms_matches_audible='unknown' if mode == 'unknown' else 'true'))
base.with_suffix('.md').write_text('LMS read-ahead OK\n')
base.with_suffix('.log').write_text('fake dry-run\n')
conn.close()
'''


class SuiteTests(unittest.TestCase):
    def launch(self, root, dryrun, *, only='alac-on', speed=1000, bonus=1):
        return subprocess.Popen([sys.executable, str(ROOT/'tests/queue_suite_runner.py'), str(root),
            str(dryrun), str(bonus), '--server', '127.0.0.1', '--speed', str(speed), '--only', only],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def unpack(self, root):
        with tarfile.open(Path(root)/'yeney-queue-suite-latest.tar.gz') as pack:
            return {name: pack.extractfile(name).read().decode() for name in pack.getnames()}

    def test_real_four_scenarios(self):
        fake = FakeLMS(real=True)
        proc = None
        try:
            with tempfile.TemporaryDirectory() as folder:
                proc = self.launch(folder, ROOT/'yeney-queue-dryrun', only=','.join(suite.SCENARIOS), speed=4, bonus=60)
                out, err = proc.communicate(timeout=260)
                self.assertEqual(proc.returncode, 0, out+'\n'+err+'\n'+repr(fake.errors))
                self.assertEqual(out.splitlines()[-1], 'SUITE OK')
                archive = self.unpack(folder)
                for name in suite.SCENARIOS:
                    for suffix in ('.md', '.tsv', '.log', '.console'):
                        self.assertIn(name+suffix, archive)
                    self.assertIn('LMS read-ahead OK', archive[name+'.md'])
                manifest = list(csv.DictReader(io.StringIO(archive['manifest.tsv']), delimiter='\t'))
                self.assertEqual(len(manifest), 16)
                for name in ('alac-on', 'flac-on'):
                    self.assertEqual(len({r['album_id'] for r in manifest if r['scenario'] == name}), 4)
                self.assertEqual(fake.prefs, fake.original)
                self.assertEqual(fake.auto_resume_count, 4)
                sets = [c[-1] for c in fake.commands if c[0] == MAC and c[1:3] == ['playerpref', 'transitionType'] and c[-1] != '?']
                self.assertEqual(sets, ['0','3','1','3','0','3','1','3'])
                self.assertEqual(sum(c[0] == MAC and c[1] == 'play' for c in fake.commands), 4)
                self.assertEqual(sum(c[0] == MAC and c[1:3] == ['playlist','clear'] for c in fake.commands), 8)
                self.assertEqual(sum(c[0] == MAC and c[1:3] == ['playerpref','transitionSmart'] and c[-1] == '0' for c in fake.commands), 2)
                self.assertEqual(len(list(Path(folder).glob('*.tar.gz'))), 2)
                self.assertIn('CLI > '+MAC+' stop', archive['suite.log'])
                self.assertIn('%3A', archive['suite.log'])
                self.assertFalse(fake.errors, fake.errors)
                print('PASS unattended suite: real dry-run/core, four native 44.1 kHz ALAC/FLAC scenarios, encoded CLI ids, automatic resume cancelled, 3 joins, 10 s overlaps, preferences restored and matched archive')
        finally:
            if proc is not None and proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try: proc.communicate(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.communicate()
            fake.close()

    def test_selection_and_overrides(self):
        fake = FakeLMS()
        try:
            cli = suite.CLI('127.0.0.1', lambda _: None)
            chosen = suite.select(cli, suite.SCENARIOS, 3, {'alac': '100', 'flac': '200'})
            self.assertEqual([t['id'] for t in chosen['alac-off']], ['100','101','102','103'])
            self.assertIn('café', chosen['alac-off'][0]['title'])
            valid = fake.tracks[0]
            for patch in ({'duration':'59.9'}, {'duration':'nan'}, {'remote':'1'},
                          {'url':'http://radio.example/stream'}, {'samplerate':'48000'}):
                self.assertFalse(suite.eligible(dict(valid, **patch)))
            fake.tracks[0]['type'] = 'aac'
            with self.assertRaises(suite.SuiteError):
                suite.select(cli, ['alac-off'], 3, {'alac':'100'})
            fake.tracks = [dict(t, samplerate='48000') for t in fake.tracks]
            with self.assertRaises(suite.SuiteError):
                suite.select(cli, ['flac-on'], 3, {})
            print('PASS suite selection: pagination, decoded Unicode titles, overrides, mixed-codec album rejection and 44.1 kHz/local/long-track filters')
        finally:
            fake.close()

    def test_failures_restore_and_archive(self):
        for mode in ('good', 'unknown', 'error', 'hang', 'interrupt', 'load-error', 'busy'):
            with self.subTest(mode=mode):
                fake = FakeLMS(fail_load=mode == 'load-error')
                try:
                    with tempfile.TemporaryDirectory() as folder:
                        binary = Path(folder)/('hang' if mode == 'interrupt' else mode)
                        binary.write_text(FAKE_DRYRUN); binary.chmod(0o755)
                        proc = self.launch(folder, binary)
                        if mode == 'interrupt':
                            self.assertTrue(fake.played.wait(5))
                            proc.send_signal(signal.SIGINT)
                        out, err = proc.communicate(timeout=12)
                        self.assertEqual(proc.returncode, 0 if mode == 'good' else 1, out+err)
                        self.assertEqual(fake.prefs, fake.original)
                        self.assertTrue(out.splitlines()[-1] == 'SUITE OK' if mode == 'good' else out.splitlines()[-1].startswith('SUITE FAILED:'))
                        self.assertIn('SUITE OK' if mode == 'good' else 'SUITE FAILED:', out)
                        archive = self.unpack(folder)
                        self.assertIn('summary.md', archive)
                        self.assertIn('manifest.tsv', archive)
                        self.assertIn('suite.log', archive)
                        if mode == 'hang': self.assertIn('scenario-timeout', archive['summary.md'])
                        if mode == 'unknown': self.assertIn('lms-audible-not-true', archive['summary.md'])
                        if mode == 'interrupt': self.assertIn('interrupted', archive['summary.md'])
                        if mode == 'busy':
                            self.assertFalse(any(c[0] == MAC for c in fake.commands))
                        self.assertFalse(fake.errors, fake.errors)
                finally:
                    fake.close()
        print('PASS suite failure handling: success/unknown/error/timeout/SIGINT/load failure exit codes, preference restoration and diagnostic archives')

    def test_synced_player_refused_before_connect(self):
        fake = FakeLMS(grouped=True)
        try:
            with tempfile.TemporaryDirectory() as folder:
                proc = self.launch(folder, '/nonexistent')
                out, err = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 1, out+err)
                self.assertIn('test-player-is-synced', out)
                self.assertEqual(fake.session_count, 0)
                self.assertFalse(any(c[0] == MAC for c in fake.commands))
                self.assertIn('summary.md', self.unpack(folder))
                self.assertEqual(fake.prefs, fake.original)
            print('PASS suite sync isolation: grouped test player refused before HELO or mutations; unrelated sync groups left alone')
        finally:
            fake.close()

    def test_lock_and_install(self):
        import fcntl
        with tempfile.TemporaryDirectory() as folder:
            with open(Path(folder)/'yeney-queue-suite.lock', 'w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
                proc = self.launch(folder, '/nonexistent')
                out, err = proc.communicate(timeout=3)
                self.assertEqual(proc.returncode, 1)
                self.assertIn('another suite is running', out)
            self.assertIn('install -m 0755 scripts/yeney-queue-suite /usr/local/bin/yeney-queue-suite', (ROOT/'Makefile').read_text())
        print('PASS suite safety: exclusive lock before CLI access; fixed full-path installation; other players never addressed')


if __name__ == '__main__':
    unittest.main(verbosity=2)
