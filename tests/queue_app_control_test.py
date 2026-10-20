"""Part 5: loopback Sonos SOAP/GENA, real core SlimProto, and LMS CLI."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
from pathlib import Path
import socketserver
import threading
import time
import unittest
from urllib.parse import quote, parse_qs, urlsplit
from queue_audible_test import Harness, HTTP, wav


class CLI(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self):
        outer = self
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                line = self.rfile.readline().decode().strip()
                assert line.startswith('02:59:51:54:00:01 '), line
                command = line[18:]
                outer.commands.append(command)
                if command.startswith('playlist index '):
                    index = int(command.split()[-1])
                    outer.h.model.streaming = outer.tokens[index] if outer.tokens else index+1
                self.wfile.write((quote(line if outer.valid else 'wrong acknowledgement', safe='')+'\n').encode()); self.wfile.flush()
                if not outer.valid: return
                if command == 'pause 1': outer.h.lms.strm('p')
                if command == 'play': outer.h.lms.strm('u')
        super().__init__(('127.0.0.1', 0), Handler)
        self.commands = []
        self.valid = True
        self.tokens = None
        self.thread = threading.Thread(target=self.serve_forever); self.thread.start()
    def close(self):
        self.shutdown(); self.server_close(); self.thread.join()


class AppControls(unittest.TestCase):
    def start(self):
        cli = CLI()
        self.addCleanup(cli.close)
        h = Harness([44100]*3, cli_port=cli.server_address[1]); cli.h = h
        self.addCleanup(h.close)
        h.sonos.follow_controls = True
        h.wait_items(1); h.state(1)
        h.until(lambda: len(h.packets('STMs')) == 1)
        h.wait_items(2)
        h.until(lambda: h.driver.command('COMPLETEITEM 2','ITEM')=='ITEM 1')
        h.model.sources = iter(()); h.model.ready = False
        h.driver.command('PLAN 2 1 1 2')
        return h, cli

    def log(self, h): return Path(h.driver.log.name).read_text()
    def identity(self, h, item):
        h.until(lambda: h.model.playing == item)
        with h.sonos.lock:
            actual = int(parse_qs(urlsplit(h.sonos.position['TrackURI']).query)['item'][0])
        self.assertEqual(actual, item)

    def test_app_controls_and_echoes(self):
        h, cli = self.start()
        h.state(1, 0, 'PAUSED_PLAYBACK')
        h.until(lambda: len(h.packets('STMp')) == 1)
        self.assertEqual(cli.commands, ['pause 1']); self.identity(h,1)
        h.state(1)
        h.until(lambda: len(h.packets('STMr')) == 1)
        self.assertEqual(cli.commands, ['pause 1','play']); self.identity(h,1)
        self.assertEqual(len(h.sonos.actions('Pause')),0)
        self.assertEqual(len(h.sonos.actions('Play')),1)
        for item in [2,1]:
            starts = len(h.packets('STMs')); decoded = len(h.packets('STMc'))
            seeks = len(h.sonos.actions('Seek'))
            h.state(item)
            h.until(lambda: len(h.packets('STMs')) == starts+1)
            h.driver.command(f'SELECT 2 {item} 1 2')
            echoes = self.log(h).count('echo suppressed LMS retained-run=s')
            source=HTTP(wav(44100,1));h.sources.append(source)
            h.lms.strm('q');h.lms.strm('s',source,fmt='p',rate=44100)
            h.until(lambda: self.log(h).count('echo suppressed LMS retained-run=s') == echoes+1)
            self.identity(h,item)
            time.sleep(.15)
            h.until(lambda: len(h.packets('STMs'))==starts+2)
            self.assertEqual(len(h.packets('STMc')),decoded+1)
            self.assertEqual(len(h.sonos.actions('Seek')),seeks)
        starts = len(h.packets('STMs')); seeks = len(h.sonos.actions('Seek'))
        urls = [i['uri'] for i in h.items()]
        decoded = len(h.packets('STMc'))
        for seconds in [6,2]:
            h.state(1,seconds)
            h.until(lambda: cli.commands[-1] == f'time {seconds}')
            h.driver.command(f'SEEK {seconds*1000}')
            echoes = self.log(h).count('echo suppressed LMS retained-run=s')
            source=HTTP(wav(44100,1));h.sources.append(source)
            h.lms.strm('q');h.lms.strm('s',source,fmt='p',rate=44100)
            h.until(lambda: self.log(h).count('echo suppressed LMS retained-run=s') == echoes+1)
            h.until(lambda: seconds*1000 <= h.elapsed() < seconds*1000+1000)
            self.identity(h,1)
        self.assertEqual([i['uri'] for i in h.items()],urls)
        self.assertEqual(len(h.packets('STMc')),decoded+2)
        self.assertEqual(len(h.sonos.actions('Seek')),seeks)
        self.assertEqual(len(h.sonos.actions('Stop')),0)
        h.until(lambda: len(h.packets('STMs'))==starts+2)
        self.assertEqual(cli.commands,['pause 1','play','playlist index 1','playlist index 0','time 6','time 2'])
        self.assertNotIn('FALLBACK',self.log(h))
        print('PASS Sonos app pause/resume, next/previous, forward/backward history seek: raw-MAC decoded CLI acknowledgements, matching LMS/audible identity, cached URLs, no echo SOAP, one STMs per acknowledged LMS run')

    def test_own_commands_and_race(self):
        h, cli = self.start()
        h.driver.command('COMMAND p')
        h.until(lambda: len(h.packets('STMp')) == 1)
        h.driver.command('COMMAND u')
        h.until(lambda: len(h.packets('STMr')) == 1)
        h.driver.command('SEEK 4000')
        h.until(lambda: h.elapsed() >= 4000)
        h.model.streaming = 2
        h.driver.command('SELECT 2 2 1 2')
        h.until(lambda: h.model.playing == 2)
        self.assertEqual(cli.commands,[])
        h.driver.command('COMMAND q')
        h.until(lambda: len(h.sonos.actions('Stop')) == 1)
        h.driver.command('COMMAND u')
        h.until(lambda: h.sonos.transport_state == 'PLAYING')
        self.assertEqual(cli.commands,[])
        # The app's newer confirmed previous supersedes the LMS selection.
        h.state(1)
        h.until(lambda: cli.commands == ['playlist index 0'])
        h.driver.command('SELECT 2 1 1 2')
        self.identity(h,1)
        h.driver.command('COMMAND p')
        h.state(1,0,'PLAYING')
        h.until(lambda: cli.commands[-1:] == ['play'])
        h.until(lambda: h.elapsed() < 1500)
        h.until(lambda: 'echo suppressed LMS strm=u' in self.log(h))
        self.assertEqual(cli.commands, ['playlist index 0', 'play'])
        self.identity(h,1)
        self.assertNotIn('FALLBACK',self.log(h))
        print('PASS Sonos own commands: state/target/seek acknowledgements never foreign; rapid LMS/app controls converge without duplicate selection or fallback')

    def test_echo_with_playlist_edit(self):
        h, cli = self.start()
        h.state(2)
        self.identity(h,2)
        self.assertEqual(cli.commands,['playlist index 1'])
        seeks = len(h.sonos.actions('Seek'))
        cli.tokens = [2,1]
        h.driver.command('SELECT 3 2 2 1') # echoed selection plus newer LMS order
        source=HTTP(wav(44100,1));h.sources.append(source)
        h.lms.strm('q');h.lms.strm('s',source,fmt='p',rate=44100)
        h.wait_items(3)
        h.state(1)
        self.identity(h,1)
        h.driver.command('SELECT 3 1 2 1')
        self.assertEqual(cli.commands,['playlist index 1','playlist index 1'])
        self.assertEqual(len(h.sonos.actions('Seek')),seeks)
        self.assertNotIn('FALLBACK',self.log(h))
        print('PASS Sonos selection echo with LMS edit: future rebuilt safely, retained previous maps occurrence token through reordered LMS indices, no extra Sonos seek')

    def test_polled_controls_and_natural_handoff(self):
        h, cli = self.start()
        h.state(1,23)
        h.until(lambda: cli.commands == ['time 23'])
        h.driver.command('SEEK 23000')
        h.state(2)
        self.identity(h,2)
        self.assertEqual(cli.commands,['time 23'])
        h.state(2,0,'PAUSED_PLAYBACK',notify=False)
        h.until(lambda: len(h.packets('STMp')) == 1)
        h.state(2,0,'PLAYING',notify=False)
        h.until(lambda: len(h.packets('STMr')) == 1)
        h.state(2,5,notify=False)
        h.until(lambda: cli.commands[-1] == 'time 5')
        h.driver.command('SEEK 5000')
        h.notify({'CurrentTrackURI':h.items()[0]['uri'],'CurrentTrack':'1','TransportState':'PLAYING'})
        h.notify({'TransportState':'PAUSED_PLAYBACK'}, sequence=1)
        time.sleep(.1)
        self.assertNotIn('FALLBACK',self.log(h))
        self.assertEqual(cli.commands,['time 23','pause 1','play','time 5'])
        self.identity(h,2)
        print('PASS Sonos polling and natural handoff: missed GENA controls mapped once, natural join sends no CLI skip, stale sequence/contradictory event inert')

    def test_seek_beyond_produced_audio(self):
        cli = CLI(); self.addCleanup(cli.close)
        h = Harness([44100], cli_port=cli.server_address[1],
                    source_options={0: dict(first=44+4*44100*4, delay=60)})
        cli.h = h; self.addCleanup(h.close)
        h.sonos.follow_controls = True
        h.wait_items(1); h.state(1)
        h.until(lambda: len(h.packets('STMs')) == 1)
        h.model.sources = iter(()); h.model.ready = False
        h.driver.command('PLAN 2 1 1')
        h.driver.command('PREPARED 2')
        old = h.items()[0]['uri']
        h.state(1,20)
        h.until(lambda: 'app seek history=rerender' in self.log(h))
        self.assertEqual(cli.commands,['time 20'])
        h.driver.command('NEWRENDER')
        h.lms.strm('f')
        source = HTTP(wav(44100,4)); h.sources.append(source)
        h.lms.strm('s',source,fmt='p',rate=44100)
        h.until(lambda: h.driver.command('READYITEM 2','ITEM') == 'ITEM 1')
        h.driver.command('SUPPLY 2 1 20000 1 2')
        h.until(lambda: h.sonos.position['Track'] == '2')
        h.until(lambda: len(h.packets('STMs')) == 2)
        h.until(lambda: h.elapsed() >= 20000)
        h.driver.command('SEEK 20000')
        self.assertNotEqual(h.sonos.position['TrackURI'],old)
        self.assertEqual(h.model.playing,1)
        self.assertEqual(cli.commands,['time 20'])
        self.assertEqual(len(h.sonos.actions('Stop')),0)
        self.assertNotIn('FALLBACK',self.log(h))
        print('PASS Sonos app seek beyond produced audio: one LMS time, fresh pending occurrence/URL, decoder refetch, TRACK_NR confirmation and source-time origin, same LMS song without echo Stop')

    def test_cli_failure_is_latched(self):
        h, cli = self.start()
        cli.valid = False
        h.state(1,0,'PAUSED_PLAYBACK')
        h.until(lambda: 'FALLBACK foreign-control' in self.log(h))
        h.notify({'TransportState':'PAUSED_PLAYBACK'})
        time.sleep(.1)
        self.assertEqual(cli.commands,['pause 1'])
        self.assertEqual(self.log(h).count('FALLBACK foreign-control'),1)
        self.assertEqual(h.items(),[])
        self.assertTrue(h.sonos.crossfade)
        print('PASS Sonos CLI acknowledgement failure: one attempt, exact decoded reply required, one release/fallback without retries')

    def test_foreign_actions(self):
        for kind in ['add','remove','reorder','clear','same-revision-content','update-only','shuffle','repeat','crossfade','source','foreign-item','group','ambiguous-jump','stop','out-of-item-seek','paused-item']:
            with self.subTest(kind=kind):
                h, cli = self.start()
                with h.sonos.lock:
                    s = h.sonos
                    if kind == 'add': s.items.append({'uri':'http://foreign.invalid/keep'}); s.update += 1
                    elif kind == 'remove': del s.items[-1]; s.update += 1
                    elif kind == 'reorder': s.items[:] = reversed(s.items); s.update += 1
                    elif kind == 'clear': s.items.clear(); s.update += 1
                    elif kind == 'same-revision-content': s.items[-1] = {'uri':'http://foreign.invalid/keep'}
                    elif kind == 'update-only': s.update += 1
                    elif kind == 'shuffle': s.play_mode = 'SHUFFLE'
                    elif kind == 'repeat': s.play_mode = 'REPEAT_ALL'
                    elif kind == 'crossfade': s.crossfade = True
                    elif kind == 'source': s.uri = 'http://foreign.invalid/radio'
                    elif kind == 'foreign-item': s.position['TrackURI'] = 'http://foreign.invalid/other'
                    elif kind == 'group': s.grouped = True
                    elif kind == 'ambiguous-jump': s.position = dict(Track='3',TrackURI=s.items[1]['uri'],RelTime='00:00:00')
                    elif kind == 'paused-item':
                        s.position = dict(Track='2',TrackURI=s.items[1]['uri'],RelTime='00:00:00')
                        s.transport_state = 'PAUSED_PLAYBACK'
                    elif kind == 'stop': s.transport_state = 'STOPPED'
                    elif kind == 'out-of-item-seek': s.position['RelTime'] = '00:01:00'
                    foreign = [dict(i) for i in s.items if 'foreign.invalid' in i['uri']]
                h.notify({'TransportState':h.sonos.transport_state})
                h.until(lambda: 'FALLBACK foreign-control' in self.log(h))
                h.notify({'TransportState':'PLAYING'})
                time.sleep(.15)
                self.assertEqual(self.log(h).count('FALLBACK foreign-control'),1)
                self.assertEqual(self.log(h).count('release reason=foreign-control'),1)
                self.assertEqual(h.items(),foreign)
                self.assertTrue(h.sonos.crossfade)
                self.assertEqual(cli.commands,[])
                print(f'PASS Sonos foreign {kind}: one release, one foreign-control fallback request, foreign entries preserved, CrossfadeMode restored')
                # End each subcase now rather than keeping all room fakes alive.
                h.close(); cli.close()
                self._cleanups.pop(); self._cleanups.pop()


if __name__ == '__main__': unittest.main()
