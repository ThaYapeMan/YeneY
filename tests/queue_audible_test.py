"""Real core/queue controller/HTTP + loopback Sonos GENA/SOAP and LMS song model."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import http.client
import io
from pathlib import Path
import struct
import sys
import threading
import time
import unittest
import wave
from xml.sax.saxutils import escape, quoteattr
from queue_controller_test import Coordinator, Driver, request, ROOT
sys.path.insert(0, str(ROOT/'third_party/yeney-core/tests'))
from fake_lms import LMS, HTTP, SongQueue


def wav(rate, seconds=24):
    out = io.BytesIO()
    with wave.open(out, 'wb') as writer:
        writer.setnchannels(2); writer.setsampwidth(2); writer.setframerate(rate)
        writer.writeframes(struct.pack('<hh', 123, -456)*rate*seconds)
    return out.getvalue()


class Harness:
    def __init__(self, rates=(44100, 44100, 44100), fade=False, cli_port=None, source_options=None):
        self.sonos = Coordinator()
        self.lms = LMS()
        self.driver = Driver(self.sonos, 'queue-audible-fixture', [self.lms.listener.getsockname()[1]] + ([] if cli_port is None else [cli_port]))
        self.lms.accept()
        self.sources = []
        def sources():
            for i, rate in enumerate(rates):
                source = HTTP(wav(rate), **((source_options or {}).get(i, {}))); self.sources.append(source)
                yield source, dict(fmt='p', rate=rate, transition=int(fade), period=10 if fade else 0)
        self.model = SongQueue(self.lms, sources())
        self.errors, self.stopped = [], False
        def read():
            try:
                while not self.stopped:
                    if self.lms.read(eof_ok=True) is None: break
            except Exception as error:
                if not self.stopped: self.errors.append(error)
        self.reader = threading.Thread(target=read, daemon=True); self.reader.start()
        self.seq = 0

    def until(self, predicate, timeout=6):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            if self.errors: raise AssertionError((self.errors, Path(self.driver.log.name).read_text()))
            if predicate(): return
            time.sleep(.01)
        raise AssertionError(Path(self.driver.log.name).read_text())

    def packets(self, event): return [p for p in self.lms.packets if p.get('event') == event]
    def items(self):
        with self.sonos.lock: return list(self.sonos.items)
    def wait_items(self, count): self.until(lambda: len(self.items()) >= count and 'resource' in self.items()[count-1])

    def state(self, track, seconds=0, state='PLAYING', uri=None, notify=True, event_overrides=None, sequence=None):
        self.wait_items(track)
        uri = uri or self.items()[track-1]['uri']
        reading = dict(Track=str(track), TrackURI=uri, RelTime=f'{seconds//3600:02}:{seconds//60%60:02}:{seconds%60:02}')
        with self.sonos.lock:
            self.sonos.position = reading
            self.sonos.transport_state = state
        if notify: self.notify(dict(CurrentTrack=reading['Track'], CurrentTrackURI=uri,
                                   RelativeTimePosition=reading['RelTime'], TransportState=state,
                                   **(event_overrides or {})), sequence)

    def notify(self, fields, sequence=None):
        if sequence is None: self.seq += 1; sequence = self.seq
        change = '<Event><InstanceID val="0">'+''.join('<'+k+' val='+quoteattr(v)+'/>' for k,v in fields.items())+'</InstanceID></Event>'
        body = '<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><LastChange>'+escape(change)+'</LastChange></e:property></e:propertyset>'
        c = http.client.HTTPConnection('127.0.0.1', self.driver.port, timeout=3)
        c.request('NOTIFY', '/avt', body, headers={'NT':'upnp:event', 'NTS':'upnp:propchange',
                  'SID':'uuid:audible-fixture', 'SEQ':str(sequence)})
        r = c.getresponse(); r.read(); assert r.status == 200, r.status; c.close()

    def elapsed(self):
        stamp = len(self.lms.packets)+12345
        self.lms.strm('t', value=stamp)
        self.until(lambda: any(p.get('stamp') == stamp for p in self.packets('STMt')))
        return next(p['elapsed'] for p in self.packets('STMt') if p.get('stamp') == stamp)

    def close(self):
        self.stopped = True
        self.driver.close()
        self.reader.join(3)
        self.lms.close()
        for source in self.sources: source.close()
        self.sonos.close()
        assert not self.errors, self.errors


class AudibleTests(unittest.TestCase):
    def test_joins(self):
        for name, rates, fade in [('44100', [44100]*3, False), ('48000', [48000]*3, False),
                                  ('mixed', [44100,48000,44100], False), ('crossfade-10s', [44100]*3, True)]:
            with self.subTest(name=name):
                h = Harness(rates, fade)
                try:
                    h.wait_items(1)
                    uri = h.items()[0]['uri']
                    # Range/prefetch produces no core audible start.
                    c,r = request(uri, byte_range='bytes=0-2047'); self.assertEqual(r.status,206); r.read(); c.close()
                    time.sleep(.12)
                    self.assertEqual(h.packets('STMs'), [])
                    for track in range(1,4):
                        h.wait_items(track)
                        before = time.monotonic()
                        h.state(track)
                        h.until(lambda: len(h.packets('STMs')) == track)
                        starts = h.packets('STMs')
                        self.assertGreaterEqual(starts[-1]['time'], before)
                        self.assertEqual(starts[-1]['playing_occurrence'], track)
                        self.assertEqual(h.model.playing, track)
                        # Per-item zero, followed immediately by a moving clock.
                        first = h.elapsed(); self.assertLess(first,500)
                        time.sleep(.12)
                        second = h.elapsed(); self.assertGreater(second, first)
                        h.state(track, 5)
                        h.until(lambda: h.elapsed() >= 5000)
                        values = [h.elapsed() for _ in range(3)]
                        self.assertEqual(values, sorted(values))
                        h.state(track, 4) # late position cannot rewind within the item
                        self.assertGreaterEqual(h.elapsed(),values[-1])
                        # End cap is the rendered asset (outgoing overlap removed).
                        h.state(track, 90)
                        length = int(h.items()[track-1]['resource']['size'])
                        cap = (length-44)//6*1000//rates[track-1]
                        h.until(lambda: h.elapsed() == cap)
                        self.assertEqual(h.elapsed(), cap)
                        h.until(lambda: len(h.packets('STMd')) >= track)
                        events = [p.get('event') for p in h.lms.packets if p.get('event') in ('STMs','STMd')]
                        self.assertLess(events.index('STMs'), events.index('STMd'))
                    self.assertEqual([p['playing_occurrence'] for p in h.packets('STMs')], [1,2,3])
                    for n in range(3):
                        self.assertLess(h.packets('STMs')[n]['time'], h.packets('STMd')[n]['time'])
                    self.assertEqual(len(h.sonos.actions('SetAVTransportURI')),1)
                    if fade:
                        self.assertEqual((int(h.items()[0]['resource']['size'])-44)//6, 14*44100)
                        self.assertIn('length=441000', Path(h.driver.log.name).read_text())
                    print(f'PASS Sonos audible {name}: confirmed STMs then STMd, LMS identity at every join, zero-based moving monotonic elapsed, rendered end cap; HTTP prefetch inert')
                except Exception:
                    print(Path(h.driver.log.name).read_text(), flush=True); raise
                finally: h.close()

    def test_events_pause_poll_and_fences(self):
        h = Harness()
        try:
            h.wait_items(1)
            # A GENA event alone is not confirmation while SOAP disagrees.
            fields = dict(CurrentTrack='1', CurrentTrackURI=h.items()[0]['uri'],
                          RelativeTimePosition='00:00:00', TransportState='PLAYING')
            h.notify(fields); time.sleep(.15)
            self.assertEqual(h.packets('STMs'), [])
            h.state(1); h.until(lambda: len(h.packets('STMs')) == 1)
            h.wait_items(2)
            h.notify(fields, sequence=1) # replay
            h.driver.command('STALE')
            foreign = dict(fields, CurrentTrackURI='http://foreign.invalid/item.wav')
            h.notify(foreign)
            time.sleep(.1); self.assertEqual(len(h.packets('STMs')), 1)
            h.state(1,2,'PAUSED_PLAYBACK'); h.until(lambda: len(h.packets('STMp')) == 1)
            frozen = h.elapsed(); time.sleep(.2); self.assertEqual(h.elapsed(), frozen)
            self.assertTrue(h.sonos.crossfade is False)
            h.state(1,2); h.until(lambda: len(h.packets('STMr')) == 1)
            time.sleep(.15); self.assertGreater(h.elapsed(), frozen)
            self.assertEqual(len(h.packets('STMs')),1)
            # Lost notification: SOAP polling is independent evidence.
            h.state(2, notify=False)
            h.until(lambda: len(h.packets('STMs')) == 2, timeout=3)
            self.assertEqual(h.model.playing,2)
            # Delayed prior item cannot become audible again.
            h.notify(fields); time.sleep(.1)
            self.assertEqual(len(h.packets('STMs')),2)
            h.state(2,3,'STOPPED'); h.until(lambda: len(h.packets('STMu')) == 1)
            frozen = h.elapsed(); time.sleep(.15); self.assertEqual(h.elapsed(),frozen)
            self.assertTrue(h.items()); self.assertFalse(h.sonos.crossfade)
            h.driver.command('RELEASE')
            h.notify(fields); time.sleep(.1)
            self.assertEqual(len(h.packets('STMs')),2)
            self.assertTrue(h.sonos.crossfade)
            log = Path(h.driver.log.name).read_text()
            for reason in ('event-position-disagreement','stale-sequence','stale-epoch','foreign-or-revoked-uri'):
                self.assertIn('reason='+reason,log)
            self.assertIn('strm_to_audible_s=',log)
            print('PASS Sonos speaker evidence: GENA/SOAP disagreement, stale epoch/sequence, foreign/revoked/old items ignored; missed GENA polled; pause/resume/stop reflected without ownership release')
        finally: h.close()


if __name__ == '__main__': unittest.main()
