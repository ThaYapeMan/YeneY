"""LMS control acceptance: real SlimProto/core, owned queue, loopback only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
from pathlib import Path
import time
import unittest
from urllib.parse import urlsplit, parse_qs
from queue_audible_test import Harness, HTTP, wav
from queue_controller_test import Coordinator, Driver


class Controls(unittest.TestCase):
    def test_initial_middle_window(self):
        speaker = Coordinator()
        driver = Driver(speaker)
        try:
            for occurrence in range(1,8): driver.put(occurrence)
            driver.command('PLAY 1 4 1 2 3 4 5 6 7')
            driver.wait(4)
            with speaker.lock:
                ids = [int(parse_qs(urlsplit(i['uri']).query)['item'][0]) for i in speaker.items]
                self.assertEqual(ids, [4,5,6,7])
                self.assertEqual(int(parse_qs(urlsplit(speaker.position['TrackURI']).query)['item'][0]),4)
            self.assertEqual(speaker.actions('Seek')[0][1]['Target'],'1')
            print('PASS LMS initial middle position: prepared selected occurrence plus three ahead, TRACK_NR then Play')
        finally:
            driver.close(); speaker.close()

    def prepared(self, h, occurrence):
        h.until(lambda: h.driver.command(f'READYITEM {occurrence}', 'ITEM') == 'ITEM 1')

    def identity(self, h, expected, asset=None):
        h.until(lambda: h.model.playing == expected)
        with h.sonos.lock:
            uri = h.sonos.position['TrackURI']
        self.assertEqual(int(parse_qs(urlsplit(uri).query)['item'][0]), asset or expected)
        time.sleep(.12)
        self.assertEqual(h.model.playing, expected)

    def select(self, h, revision, current, ids, asset=None):
        h.model.streaming = current
        count = len(h.packets('STMs'))
        decoded_starts = len(h.packets('STMc'))
        boundaries = Path(h.driver.log.name).read_text().count('boundary frame=')
        h.driver.command(f'SELECT {revision} {current} '+' '.join(map(str, ids)))
        # A cached run still performs the real LMS HTTP/RESP handshake,
        # without decoding/replacing its retained PCM.
        source = HTTP(wav(44100, 1)); h.sources.append(source)
        h.lms.strm('s',source,fmt='p',rate=44100)
        h.until(lambda: len(h.packets('STMs')) == count+1)
        self.identity(h, current, asset)
        self.assertEqual(len(h.packets('STMc')), decoded_starts+1)
        self.assertEqual(Path(h.driver.log.name).read_text().count('boundary frame='),boundaries)
        time.sleep(.15)
        self.assertEqual(len(h.packets('STMs')), count+1)

    def test_controls_edits_and_history(self):
        h = Harness([44100]*6, fade=True)
        h.sonos.follow_controls = True
        try:
            # Establish speaker-confirmed initial position 0, then populate
            # retained history and three successors using real LMS read-ahead.
            for track in range(1, 4):
                h.wait_items(track); h.state(track)
                h.until(lambda: len(h.packets('STMs')) == track)
                self.identity(h, track)
            h.wait_items(4)
            # No further automatic advance while explicit LMS control owns intent.
            h.model.sources = iter(())
            h.model.ready = False
            ids = [1,2,3,4]
            h.driver.command('PLAN 2 3 1 2 3 4')
            h.lms.strm('p')
            h.until(lambda: len(h.packets('STMp')) == 1)
            frozen = h.elapsed()
            for _ in range(10):
                time.sleep(1)
                self.assertEqual(h.elapsed(), frozen)
            self.identity(h,3)
            h.lms.strm('u')
            h.until(lambda: len(h.packets('STMr')) == 1)
            h.until(lambda: h.elapsed() > frozen)
            self.identity(h,3)
            print('PASS LMS pause 10 s/resume: native Pause/Play, speaker-confirmed STMp/STMr once, frozen clock and identity')
            self.select(h,2,4,ids) # manual next into cached 10 s mix
            reported = len(h.packets('STMd'))
            self.select(h,2,3,ids) # retained previous; no decoder request
            self.select(h,2,1,ids) # play position 0
            self.select(h,2,3,ids) # play middle
            h.until(lambda: len(h.packets('STMd')) == reported+3)
            log = Path(h.driver.log.name).read_text()
            self.assertIn('cached=1 mixed-intro=1', log)
            print('PASS LMS play position 0/middle, manual next/previous cached 10 s mixed intro: matching audible identity, one STMs per selection, no double advance')
            starts = len(h.packets('STMs'))
            h.driver.command('SEEK 2000')
            h.until(lambda: 2000 <= h.elapsed() < 3500)
            self.assertEqual(len(h.packets('STMs')),starts)
            self.identity(h,3)
            print('PASS LMS seek inside produced history: REL_TIME, confirmed elapsed reanchor, no duplicate STMs')
            # Eviction uses a fresh decoder run; retained previous above never did.
            source = HTTP(wav(44100, 24)); h.sources.append(source)
            h.driver.command('NEWRENDER')
            h.lms.strm('s', source, fmt='p', rate=44100)
            self.prepared(h,5)
            h.driver.command('EVICT 1')
            h.driver.command('PREPARED 5')
            self.select(h,2,1,ids,5)
            print('PASS LMS previous after history eviction: explicit re-render, fresh occurrence/URL, confirmed LMS identity')
            self.select(h,2,3,ids)
            starts = len(h.packets('STMs'))
            # Decode a fresh source run for an unproduced target. The fixture's
            # preparation callback adopts this run with a fresh occurrence/URL.
            # Current rendered item is 14 s after its outgoing 10 s overlap;
            # LMS source seek 20 s is still inside the original 24 s track.
            source = HTTP(wav(44100, 4)); h.sources.append(source)
            h.driver.command('NEWRENDER')
            h.lms.strm('s', source, fmt='p', rate=44100)
            self.prepared(h,6)
            h.driver.command('PREPARED 6')
            h.driver.command('SEEK 20000')
            h.model.streaming = 3
            h.until(lambda: len(h.packets('STMs')) == starts+1)
            self.identity(h,3,6)
            h.until(lambda: h.elapsed() >= 20000)
            print('PASS LMS seek beyond produced audio: fresh rendered occurrence and URL, TRACK_NR, source-time origin, audible identity')
            source = HTTP(wav(44100, 8)); h.sources.append(source)
            h.driver.command('NEWRENDER')
            h.lms.strm('s', source, fmt='p', rate=44100)
            self.prepared(h,7)
            # Add/delete/move/clear retain the exact audible URI and never Seek.
            for revision, current, edited in [(3,3,[1,2,3,4,7]), (4,3,[1,3,4,7]),
                                               (5,3,[7,1,3,4]), (6,4,[1,4,7]), (7,0,[])]:
                with h.sonos.lock: before = dict(h.sonos.position)
                seeks = len(h.sonos.actions('Seek'))
                h.driver.command(f'PLAN {revision} {current} '+' '.join(map(str,edited)))
                with h.sonos.lock: self.assertEqual(h.sonos.position,before)
                self.assertEqual(len(h.sonos.actions('Seek')),seeks)
                self.identity(h,3,6)
                if revision == 6:
                    with h.sonos.lock:
                        track = int(h.sonos.position['Track'])
                        future = h.sonos.items[track:]
                        self.assertEqual([int(parse_qs(urlsplit(i['uri']).query)['item'][0]) for i in future], [4,7])
                added = len(h.sonos.actions('AddURIToQueue'))
                h.driver.command(f'PLAN {revision} {current} '+' '.join(map(str,edited)))
                self.assertEqual(len(h.sonos.actions('AddURIToQueue')), added)
            print('PASS LMS playlist add/delete/move/clear: token-based future-only rebuilds, audible URI retained, no transport skip')
            stops=len(h.sonos.actions('Stop'));underruns=len(h.packets('STMu'))
            h.lms.strm('q'); h.until(lambda: len(h.sonos.actions('Stop'))>stops and h.sonos.transport_state=='STOPPED')
            self.assertEqual(len(h.packets('STMu')),underruns)
            self.assertTrue(h.items()); self.assertFalse(h.sonos.crossfade)
            self.identity(h,3,6)
            h.lms.strm('f'); time.sleep(.2)
            self.assertEqual(len(h.packets('STMu')),underruns)
            print('PASS LMS stop/flush: Stop retains ownership, confirmed Stop/STMf without an unsolicited underrun, no double advance')
        finally: h.close()


if __name__ == '__main__': unittest.main()
