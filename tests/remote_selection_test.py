"""Remote menu selection/retry tests: in-memory LMS replies and clocks only."""
import io
import json
from pathlib import Path
import runpy
import tempfile
import os
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
import unittest
ROOT=Path(__file__).resolve().parents[1]
M=runpy.run_path(str(ROOT/'scripts/yeney-queue-device-suite'))
Suite=M['Suite']
class Menus:
    def __init__(self, rows):self.rows=rows;self.calls=[]
    def call(self,*args):
        self.calls.append(args)
        ident=next((a[8:] for a in args if a.startswith('item_id:')),None)
        return [k+':'+v for row in self.rows.get((args[1],ident),[]) for k,v in row.items()]
class Clock:
    def __init__(self,journal=''):self.time=0;self.journal=journal;self.calls=[]
    def now(self):return self.time
    def sleep(self,n):self.time+=n
    def run(self,*args):self.calls.append(args);return self.journal

def selector(rows,remote=None):
    s=Suite.__new__(Suite);s.mac='00:11:22:33:44:55';s.cli=Menus(rows);s.remote_argument=remote;s.remote_pool=[]
    return s
class Tests(unittest.TestCase):
    def test_feed_favourite_and_containers(self):
        feed='https://plus.bnr.nl/program.rss?t=token'
        s=selector({('favorites',None):[dict(id='feed',type='audio',name='Boekestijn & De Wijk | Plus',url=feed)],
                    ('favorites','feed'):[dict(id='outline',type='outline',url='https://radio.invalid/ignore.mp3'),
                                         dict(id='first',type='audio',title='First episode',url='podcast://https://audio.invalid/episode.mp3')]})
        t,source=s.remote_choice()
        self.assertEqual(t['url'],'podcast://https://audio.invalid/episode.mp3');self.assertEqual(source,'favorites')
        for suffix in ('.rss?t=x','.xml','.opml','.atom','.m3u','.m3u8','.pls','.asx'):
            self.assertTrue(s.remote_container(dict(url='https://feed.invalid/a'+suffix)))
        self.assertTrue(s.remote_container(dict(url='https://audio.invalid/file.mp3',hasitems='1')))
        print('PASS remote selection: RSS favourite is browsed to first episode; feed/playlist/container URLs never loaded')
    def test_explicit_name_and_episode_url(self):
        rows={('favorites',None):[dict(id='other',type='audio',url='https://other.invalid/live',name='Other'),
                                dict(id='feed',type='audio',url='https://plus.bnr.nl/a.rss?t=x',name='Requested')],
              ('favorites','feed'):[dict(id='ep',url='https://audio.invalid/first.mp3',type='audio')]}
        for arg in ('Requested','https://plus.bnr.nl/a.rss?t=x'):
            s=selector(rows,arg);t,_=s.remote_choice();self.assertEqual(t['url'],'https://audio.invalid/first.mp3')
            self.assertEqual(len(s.remote_pool),1)
        s=selector({},'podcast://https://audio.invalid/explicit.mp3');t,source=s.remote_choice()
        self.assertEqual(t['url'],s.remote_argument);self.assertEqual(source,'explicit --remote');self.assertFalse(s.cli.calls)
        self.assertIn("parser.add_argument('--remote'",(ROOT/'scripts/yeney-queue-device-suite').read_text())
        print('PASS --remote: exact favourite name, feed URL and explicit episode URL honoured without choosing another favourite')
    def test_strm_deadline(self):
        s=Suite.__new__(Suite);s.unit='yeney@Study.service';s.pid=42;s.backend=Clock();s.current=dict(name='remote-source')
        with self.assertRaisesRegex(M['SuiteError'],'LMS did not start: no strm s within 10 s'):
            s.wait_strm(123,0)
        self.assertEqual(s.backend.now(),10)
        self.assertTrue(all('--since' in c and '-u' in c and '_PID=42' in c for c in s.backend.calls))
        s.backend=Clock(json.dumps(dict(__MONOTONIC_TIMESTAMP=2000000,MESSAGE='core: queue command strm s')))
        self.assertEqual(s.wait_strm(123,0),2)
        print('PASS startup journal: room/process scoped strm s timestamp; no strm s fails as LMS did not start after 10 s')
    def scenario(self,fail_count,count=2):
        temp=tempfile.TemporaryDirectory(prefix='yeney-remote-selection-');self.addCleanup(temp.cleanup)
        s=Suite.__new__(Suite);s.work=Path(temp.name);s.backend=Clock();s.remote_note='favorites';s.results=[];s.actions=[]
        s.remote_pool=[dict(id='',url='https://audio.invalid/'+str(i)+'.mp3',title=str(i),samplerate='',duration='0') for i in range(count)]
        s.selected={'remote-source':s.remote_pool[:1],'album':[dict(url='file:///local.flac')]}
        def begin(name):s.current=dict(name=name,verdict='PASS');s.results.append(s.current)
        def load(tracks,**kwargs):
            s.actions.append(('load',tracks[0]['url'],kwargs))
            if not tracks[0]['url'].startswith('file:') and int(tracks[0]['title'])<fail_count:
                raise M['SuiteError']('LMS did not start: no strm s within 10 s')
        def sample(*args):s.backend.sleep(1)
        s.begin=begin;s.load=load;s.sample=sample;s.log=lambda _:None
        s.mutate=lambda *args:s.actions.append(args);s.pending_mutation=s.mutate
        return s
    def test_unplayable_fails(self):
        s=self.scenario(1)
        with self.assertRaisesRegex(M['SuiteError'],'LMS did not start'):s.remote_scenario()
        self.assertEqual(len([a for a in s.actions if a[0]=='load']),1)
        print('PASS no-strm remote start is a failure; no retry can hide it')
    def test_empty_menu_skips(self):
        s=self.scenario(0,0);s.remote_scenario()
        self.assertEqual(s.results[0]['verdict'],'SKIP')
        self.assertFalse(s.actions)
        s=selector({('favorites',None):[dict(id='feed',type='audio',url='https://a.invalid/feed.rss')]})
        self.assertIsNone(s.remote_choice()[0]);self.assertFalse(s.remote_pool)
        print('PASS empty/feed-only menu explicitly reports SKIP before loading anything')
if __name__=='__main__':unittest.main(verbosity=2)
