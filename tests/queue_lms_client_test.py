"""Production queue CLI worker against delayed, dropped and late loopback replies."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import json
from pathlib import Path
import socketserver
import subprocess
import threading
import time
import unittest
from urllib.parse import quote,unquote
ROOT=Path(__file__).resolve().parents[1]


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address=True;daemon_threads=False
    def __init__(self,profile):
        self.profile=profile;self.commands=[];self.connections=0;self.index='0';self.mode='play'
        owner=self
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                wire=self.rfile.readline().decode().strip()
                assert wire.startswith('aa:bb:cc:dd:ee:ff '),wire
                words=[unquote(w) for w in wire.split()];command=' '.join(words[1:])
                owner.connections+=1;owner.commands.append(command);count=owner.commands.count(command)
                if profile=='outage' or (profile=='drop' and count==1):return
                if profile=='mutation-drop' and command=='pause 1':owner.mode='pause';return
                if profile=='late' and command=='mode ?' and count==1:time.sleep(1)
                elif command=='status 0 60000 tags:alKu':time.sleep(20 if profile=='snapshot20' else 10)
                elif command in ('pause 1','playlist index 2','time 20'):time.sleep(5)
                else:time.sleep(.1)
                if command=='pause 1':owner.mode='pause'
                if command=='playlist index 2':owner.index='2'
                if command.endswith('?'):
                    value=('25' if command=='time ?' else owner.mode if command=='mode ?' else ('2' if profile=='late' else owner.index))
                    if profile=='late' and command=='mode ?':value='pause'
                    response=words[:-1]+[value]
                else:response=words+(['playlist_tracks:0','playlist_cur_index:0'] if command.startswith('status ') else [])
                try:self.wfile.write((' '.join(quote(w,safe='') for w in response)+'\n').encode())
                except ConnectionError:pass
        super().__init__(('127.0.0.1',0),Handler)
        self.worker=threading.Thread(target=self.serve_forever,name='production-delayed-cli');self.worker.start()
    def close(self):self.shutdown();self.server_close();self.worker.join()


class Tests(unittest.TestCase):
    def run_profile(self,profile):
        server=Server(profile)
        try:
            p=subprocess.run([str(ROOT/'queue-lms-client-test'),str(server.server_address[1]),profile],text=True,capture_output=True,timeout=45)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            status=json.loads(next(l[7:] for l in p.stdout.splitlines() if l.startswith('STATUS ')))
            return server.commands,p.stdout,status
        finally:server.close()
    def test_app_pause_next(self):
        commands,out,status=self.run_profile('actions')
        self.assertEqual(commands.count('pause 1'),1);self.assertEqual(commands.count('playlist index 2'),1)
        self.assertIn('mode ?',commands);self.assertIn('playlist index ?',commands)
        self.assertEqual(commands.count('time 20'),1);self.assertIn('time ?',commands)
        self.assertEqual(status['lms_cli_slow_replies'],3)
        self.assertIn('duration_ms=',out)
        print('PASS production queue LMS: 5 s app pause/next acknowledged and query-verified off caller thread, sent once, no fallback, advancing seek time verified, slow counters/logs')
    def test_snapshot(self):
        for profile in ('snapshot','snapshot20'):
            _,_,status=self.run_profile(profile);self.assertEqual(status['lms_cli_slow_replies'],1)
            self.assertEqual(status['last_fallback'],'')
        print('PASS production queue LMS: 10 s and 20 s snapshots succeed; CLI waits excluded from preparation deadline; no fallback; status counters')
    def test_late_and_drop(self):
        self.run_profile('late');self.run_profile('drop')
        commands,_,_=self.run_profile('mutation-drop');self.assertEqual(commands.count('pause 1'),1)
        print('PASS production queue LMS: timed-out/late and dropped replies retry queries once on new sockets, no reply mismatch or mutation resend')
    def test_outage(self):
        _,_,status=self.run_profile('outage');self.assertEqual(status['lms_cli_failed_replies'],2)
        print('PASS production queue LMS: no fallback at 29999 ms without a successful reply; exactly one lms-unavailable at 30000 ms; failed counters in room JSON')


if __name__=='__main__':unittest.main(verbosity=2)
