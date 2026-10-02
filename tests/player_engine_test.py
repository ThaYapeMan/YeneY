"""Real YeneY, loopback Sonos SOAP/HTTP and yeney-core's fake LMS wire driver."""
import os
import zlib
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from urllib.parse import urlsplit
import http.client

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'third_party/yeney-core/tests'))
from fake_lms import LMS, HTTP, pcm
# Reuse the existing mock's wire implementation without running its test driver.
mock = {'__file__': str(ROOT/'tests/upnp_mock_test.py')}
exec((ROOT/'tests/upnp_mock_test.py').read_text().split('\ndef run(mode, command, golden=None):')[0], mock)
Speaker, SOAP, ET, escape = (mock[k] for k in ('Speaker', 'SOAP', 'ET', 'escape'))

class WireLMS(LMS):
    def read(self, eof_ok=False):
        header = self.exact(8, eof_ok)
        if header is None: return None
        op = header[:4].decode(); body = self.exact(struct.unpack('!I', header[4:])[0])
        p = dict(op=op, body=body, time=time.monotonic())
        if op == 'STAT':
            assert len(body) == 53
            p.update(event=body[:4].decode(), elapsed=struct.unpack_from('!I', body, 43)[0],
                     stamp=struct.unpack_from('!I', body, 47)[0])
        self.packets.append(p)
        self.react(p)
        return p

class Device(Speaker):
    def handle_post(self):
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        if self.server.delay:
            self.server.delay_started.set()
            time.sleep(self.server.delay)
        if action in ('GetPositionInfo', 'GetTransportInfo', 'GetMediaInfo'):
            self.rfile.read(int(self.headers['Content-Length']))
            if action == 'GetPositionInfo':
                seconds = self.server.position
                values = f'<RelTime>0:{seconds//60:02}:{seconds%60:02}</RelTime><TrackDuration>0:01:00</TrackDuration><TrackMetaData></TrackMetaData>'
            elif action == 'GetTransportInfo':
                values = f'<CurrentTransportState>{self.server.state}</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus>'
            else: values = '<CurrentURI>' + escape(self.server.current_uri) + '</CurrentURI>'
            self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action}Response xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">{values}</u:{action}Response></s:Body></s:Envelope>')
            return
        super().handle_post()
        if action in ('Stop', 'Pause'): self.server.state = 'STOPPED' if action == 'Stop' else 'PAUSED_PLAYBACK'
        if action == 'Play':
            self.server.state = 'PLAYING'
            self.server.position = 0
            self.server.read_audio()

class Run:
    def __init__(self, mode, directory):
        self.base = directory/mode; self.base.mkdir()
        self.lms = WireLMS(); self.sources = []; self.captures = []; self.clients = []
        self.device = ThreadingHTTPServer(('127.0.0.1', 1400), Device)
        for key, value in dict(mode='ab', counts={}, requests=[], errors=[], golden={}, current_uri='',
                               position=0, state='STOPPED', delay=0).items(): setattr(self.device, key, value)
        self.device.read_audio = self.read_audio
        self.device.delay_started = threading.Event()
        self.worker = threading.Thread(target=self.device.serve_forever, daemon=True); self.worker.start()
        self.log = open(self.base/'bridge.log', 'w')
        port = self.lms.listener.getsockname()[1]
        self.proc = subprocess.Popen([str(ROOT/'yeney'), '--room=Study', '--ip=127.0.0.1', f'--server=127.0.0.1:{port}'],
                stdout=self.log, stderr=self.log, env={**os.environ, 'YENEY_POLL': 'legacy'})
        hello = self.lms.accept(); assert hello['op'] == 'HELO', hello
        self.mac = hello['body'][2:8]
        maps = Path(f'/proc/{self.proc.pid}/maps').read_text()
        assert all(library not in maps for library in ('libmad.so','libmpg123.so','libvorbis.so','libfaad.so','libasound.so')), maps
        self.lms.send('setd', bytes([0]))
        assert self.lms.wait('SETD')['body'] == b'\0Study (Sonos)\0'
    def read_audio(self):
        url = self.device.current_uri
        if not url: return
        capture = bytearray(); self.captures.append((url, capture))
        def read():
            conn = http.client.HTTPConnection(urlsplit(url).netloc, timeout=12)
            self.clients.append(conn)
            try:
                u = urlsplit(url); conn.request('GET', u.path + '?' + u.query)
                response = conn.getresponse(); assert response.status == 200, response.status
                while True:
                    data = response.read1(65536)
                    if not data: break
                    capture.extend(data)
            except (OSError, http.client.HTTPException): pass
            finally: conn.close()
        threading.Thread(target=read, daemon=True).start()
    def start(self, body, fmt='f', rate=44100, bits=16, continuation=None):
        source = HTTP(body); self.sources.append(source)
        self.start_packet = len(self.lms.packets)
        self.lms.strm('s', source, fmt=fmt, rate=rate, bits=bits)
        self.lms.wait('STMs', 10)
        if continuation is not None:
            self.decoded_track()
            self.start(continuation, fmt='p')
        elapsed = [self.lms.timer(stamp)['elapsed'] for stamp in (41, 42, 43)]
        assert elapsed == sorted(elapsed) and elapsed[0] == 0, elapsed
    def decoded_track(self):
        if not any(p.get('event') == 'STMd' for p in self.lms.packets[self.start_packet:]):
            self.lms.wait('STMd')
    def completed(self):
        if not any(p.get('event') == 'STMu' for p in self.lms.packets[self.start_packet:]):
            self.lms.wait('STMu', 12)
    def until(self, predicate, timeout=8):
        end = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() > end: raise AssertionError((self.base/'bridge.log').read_text())
            time.sleep(.01)
    def close(self, sig=signal.SIGTERM):
        self.proc.send_signal(sig)
        try: self.proc.wait(6)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait(); raise
        self.log.close()
        for client in self.clients: client.close()
        for source in self.sources: source.close()
        self.lms.close(); self.device.shutdown(); self.device.server_close(); self.worker.join()
        assert self.proc.returncode == 0, (self.base/'bridge.log').read_text()
    def decoded(self):
        result = []
        self.initial_pcm = b''
        for i, (url, data) in enumerate(self.captures):
            path = self.base/f'{i}.flac'; path.write_bytes(data)
            decoded = subprocess.run(['flac', '-s', '-d', '--decode-through-errors', '--force-raw-format',
                                      '--endian=little', '--sign=signed', '-c', str(path)], capture_output=True)
            if i == 0:
                self.initial_pcm = decoded.stdout
            if 'stream=1' not in url:
                assert decoded.stdout, (url, len(data), decoded.stderr)
                result.append(decoded.stdout)
        return result

with tempfile.TemporaryDirectory(prefix='yeney-engine-ab-') as tmp:
    directory = Path(tmp)
    raw, _ = pcm(seconds=3)
    rawpath = directory/'audio.raw'; rawpath.write_bytes(raw)
    flac = directory/'audio.flac'
    subprocess.run(['flac', '-s', '--force-raw-format', '--endian=little', '--sign=signed', '--channels=2',
                    '--bps=16', '--sample-rate=44100', '-o', str(flac), str(rawpath)], check=True)
    raw48, _ = pcm(rate=48000, bits=24, seconds=3)
    (directory/'audio48.raw').write_bytes(raw48)
    flac48 = directory/'audio48.flac'
    subprocess.run(['flac', '-s', '--force-raw-format', '--endian=little', '--sign=signed', '--channels=2',
                    '--bps=24', '--sample-rate=48000', '-o', str(flac48), str(directory/'audio48.raw')], check=True)
    marker, _ = pcm(seconds=1)
    marker = marker[:4096*4]
    marker24 = b''.join(b'\0'+marker[i:i+2] for i in range(0,len(marker),2))
    runs = []
    for mode in ('core',):
        run = Run(mode, directory)
        try:
            run.start(flac.read_bytes())
            run.until(lambda: bool(run.captures))
            assert run.lms.timer()['elapsed'] == 0
            time.sleep(1.2)
            run.device.position = 1
            time.sleep(1.1)
            elapsed = [run.lms.timer(stamp)['elapsed'] for stamp in (31,32,33)]
            assert elapsed == sorted(elapsed) and elapsed[-1] > 0, elapsed
            run.lms.strm('p'); run.lms.wait('STMp')
            run.until(lambda: run.device.counts.get('Stop', 0) == 1)
            run.lms.strm('u'); run.lms.wait('STMr')
            run.until(lambda: run.device.counts.get('Play', 0) >= 2)
            # A sync delay must never produce device transport.
            before = (run.device.counts.get('Stop', 0), run.device.counts.get('Pause', 0))
            run.lms.strm('p', value=100)
            time.sleep(.15); run.lms.timer()
            assert before == (run.device.counts.get('Stop', 0), run.device.counts.get('Pause', 0))
            run.lms.strm('p'); run.lms.wait('STMp')
            run.lms.strm('q'); run.lms.wait('STMf')
            run.start(flac.read_bytes())
            run.decoded_track()
            run.start(flac.read_bytes())  # same-rate continuation, stream 2 retained
            run.decoded_track()
            run.start(flac48.read_bytes(), rate=48000, bits=24)  # stream 3
            run.until(lambda: 'stream=3' in run.device.current_uri)
            time.sleep(3)  # the mock speaker reaches the end at a whole-second position
            run.device.position = 3
            ended = time.monotonic()
            drained = run.lms.wait('STMu', 3)
            assert drained['time'] - ended <= 3

            # Replay a compressed fixture through the same real output path.
            run.lms.strm('q'); run.lms.wait('STMf')
            run.start((ROOT/'tests/fixtures/lame-0.mp3').read_bytes(), fmt='m', continuation=marker)
            run.completed()
            time.sleep(.3)
            # A second independently tagged MP3 checks exact gapless lengths.
            run.lms.strm('q'); run.lms.wait('STMf')
            run.start((ROOT/'tests/fixtures/lame-70130.mp3').read_bytes(), fmt='m', continuation=marker)
            run.completed()
            time.sleep(.3)
            run.lms.strm('q'); run.lms.wait('STMf')
            run.start(raw, fmt='p')
            run.completed()
            time.sleep(.3)
            alac = ROOT/'third_party/yeney-core/tests/fixtures/alac-16-2.m4a'
            reference = zlib.decompress(alac.with_suffix('.reference.zlib').read_bytes())
            alac_pcm = b''.join(reference[i+2:i+4] for i in range(0,len(reference),4))
            run.lms.strm('q'); run.lms.wait('STMf')
            run.start(alac.read_bytes(),fmt='l')
            run.completed()
            time.sleep(.3)
            if mode == 'core':
                run.device.delay = 2
                run.lms.strm('p'); run.lms.wait('STMp')
                assert run.device.delay_started.wait(3), 'no delayed SOAP request observed'
                latencies = []
                for stamp in range(15):
                    at = time.monotonic(); run.lms.timer(stamp + 1000)
                    latencies.append(time.monotonic() - at); time.sleep(.02)
                assert max(latencies) < .1, latencies
                run.device.delay = 0
                print(f'PASS: core delayed-SOAP heartbeat max latency {max(latencies)*1000:.1f} ms (<100 ms)')
        except Exception:
            print((run.base/'bridge.log').read_text(), flush=True)
            raise
        finally:
            run.close()
        log = (run.base/'bridge.log').read_text()
        decisions = re.findall(r'^strm [^\n]+: decision=[^\n]+', log, re.MULTILINE)
        # The additional core-only nonblocking probe ends with p.
        if mode == 'core': decisions.pop()
        streams = re.findall(r'^stream (\d+): FLAC (\d+)-bit (\d+) Hz', log, re.MULTILINE)
        assert streams == [('1','24','44100'),('2','24','44100'),('3','24','48000'),('4','24','44100'),('5','24','44100'),('6','24','44100'),('7','24','44100')], streams
        runs.append((run.mac, run.decoded(), decisions, run.initial_pcm))
        print(f'PASS: {mode} real binary FLAC/MP3, pause/resume, paused seek, continuous/rate-change IDs, sync delay, audible clock and playlist completion within 3 s')
    decoded=runs[0][1]
    assert len(decoded)==6
    def padded(raw): return b''.join(b'\0'+raw[i:i+2] for i in range(0,len(raw),2))
    # Retain the core's lossless checks against source PCM, independent of another engine.
    for index,reference in ((0,padded(raw)*2),(1,raw48),(4,padded(raw)),(5,padded(alac_pcm))):
        assert decoded[index]==reference[:len(decoded[index])], ('lossless mismatch',index)
        assert decoded[index],index
    initial=runs[0][3]
    assert initial and initial==padded(raw)[:len(initial)]
    for index,frames in ((2,70130),(3,80060)):
        data=decoded[index]
        assert len(data)>frames*6,(index,len(data)//6,frames)
        assert data[frames*6:]==marker24[:len(data)-frames*6],('gapless boundary',index)
        # The bundled decoder suite compares minimp3 output to independent
        # PCM reference fixtures; keep exact integration length/marker assertions.
        print(f'PASS: core MP3 {frames} frames: exact gapless trimming and marker boundary',flush=True)
    print('PASS: core lossless FLAC/PCM/ALAC matches source PCM; no removed codec libraries loaded')

    for mode in ('core',):
        parent = directory/f'manual-{mode}'; parent.mkdir()
        run = Run(mode, parent)
        assert run.mac == runs[0][0], 'player identity changed across sessions'
        try:
            run.lms.rebuffer_delay = .4
            for track in range(4):
                if track:
                    for _ in range(2):
                        run.lms.strm('q'); run.lms.wait('STMf')
                before_stop = run.device.counts.get('Stop', 0)
                before_play = run.device.counts.get('Play', 0)
                run.start(flac.read_bytes())
                run.until(lambda: run.device.counts.get('Play', 0) == before_play + 1)
                time.sleep(.5)
                run.lms.timer()
                assert run.device.counts.get('Stop', 0) == before_stop, run.device.counts
                assert run.device.counts.get('Play', 0) == before_play + 1, run.device.counts
                assert not run.lms.rebuffer_commands, run.lms.rebuffer_commands
            assert not any(p.get('event') == 'STMo' for p in run.lms.packets)
            print(f'PASS: {mode} repeated q/q/s and normal starts: no STMo, rebuffer p/u or extra UPnP Stop/PlayStream')
        finally:
            run.close()

    for sig in (signal.SIGINT, signal.SIGQUIT, signal.SIGHUP):
        parent = directory/f'signal-{sig}'; parent.mkdir()
        run = Run('core', parent)
        run.close(sig)
        print(f'PASS: core clean shutdown on {sig.name}, including atexit HTTP/event cleanup')
