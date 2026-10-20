"""Part 6 boundary: real queue/controller/HTTP and loopback Sonos/LMS CLI.
The production binary acceptance and opening of its guard belong to part 7.
"""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import json
import socketserver
import threading
import time
import unittest
from urllib.parse import quote
from queue_controller_test import Coordinator, Driver, request, expected

class CLI(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self):
        outer = self
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                line = self.rfile.readline().decode().strip()
                assert line.startswith('02:59:51:54:00:01 '), line
                outer.commands.append(line[18:])
                self.wfile.write((quote(line, safe='')+'\n').encode())
        super().__init__(('127.0.0.1',0), Handler)
        self.commands=[]
        self.worker=threading.Thread(target=self.serve_forever); self.worker.start()
    def close(self):
        self.shutdown(); self.server_close(); self.worker.join()

class Modes(unittest.TestCase):
    def status(self,d):
        line=d.command('MODE','MODE ')
        body,releases,switches=line[5:].rsplit(' ',2)
        return json.loads(body),int(releases),int(switches)
    def test_detected_foreign_edit_preserves_foreign_entries(self):
        s=Coordinator();cli=CLI();d=Driver(s,'queue-mode-fixture',(cli.server_address[1],))
        try:
            s.follow_controls=True;d.put(1);d.command('PLAY 1 1 1');d.wait(1)
            time.sleep(1.1);d.command('EVIDENCE 200')
            s.add_foreign();foreign=s.items[-1].copy()
            time.sleep(1.1);d.command('EVIDENCE 300')
            state,release,switch=self.status(d)
            self.assertEqual((state['last_fallback'],release,switch),('foreign-control',1,1))
            self.assertEqual(s.items,[foreign]);self.assertTrue(s.crossfade)
            d.command('EVIDENCE 400');self.assertEqual(self.status(d)[1:],(1,1))
            print('PASS managed foreign edit: detected by UpdateID; exact-owned removal preserves foreign entry; one fallback/release',flush=True)
        finally:d.close();cli.close();s.close()

    def test_triggers_tail_handoff_and_fences(self):
        for reason in ['foreign-control','preparation-timeout','soap-error','length-fault','storage-fault','coordinator-change','lms-unavailable']:
            with self.subTest(reason=reason):
                s=Coordinator(); cli=CLI(); d=Driver(s,'queue-mode-fixture',(cli.server_address[1],))
                try:
                    s.follow_controls=True
                    for i in range(1,5):d.put(i)
                    d.command('PLAY 1 1 1 2 3 4');d.wait(4)
                    with s.lock:s.position['RelTime']='00:00:02'
                    time.sleep(1.1);d.command('EVIDENCE 200')
                    uri=s.items[0]['uri'];conn,response=request(uri);self.assertEqual(response.status,200)
                    # Hold an HTTP reader across release. It may complete its
                    # immutable old body, but cannot report audible progress.
                    prefix=response.read(100)
                    d.command(f'REQUEST {reason} 300')
                    if reason not in ['foreign-control','coordinator-change']:
                        state,release,switch=self.status(d)
                        self.assertEqual((state['state'],release,switch),('drain',0,0))
                        self.assertEqual(len(s.actions('SetAVTransportURI')),1)
                        self.assertEqual(len(s.items),1)
                        self.assertEqual(cli.commands,[])
                        with s.lock:s.position['RelTime']='00:00:08';s.transport_state='STOPPED'
                        time.sleep(1.1);d.command('EVIDENCE 600')
                    state,release,switch=self.status(d)
                    self.assertEqual((state['active'],state['state'],state['last_fallback'],release,switch),('stream','handoff',reason,1,1))
                    self.assertTrue(s.crossfade)
                    self.assertEqual(s.items,[])
                    self.assertEqual(cli.commands,['playlist index 4','time 2'] if reason in ['foreign-control','coordinator-change'] else ['playlist index 5'])
                    self.assertEqual(prefix+response.read(),expected(1,44100));conn.close()
                    conn,response=request(uri);self.assertEqual(response.status,404);response.read();conn.close()
                    d.command('LATE')
                    d.command(f'REQUEST {reason} 700')
                    d.command(f'AUDIBLE {state["generation"]-1} 800')
                    self.assertEqual(self.status(d)[0]['state'],'handoff')
                    d.command(f'AUDIBLE {state["generation"]} 900')
                    state,release,switch=self.status(d)
                    self.assertEqual((release,switch,state['state']),(1,1,'active'))
                    self.assertGreaterEqual(state['handoff_gap_ms'],300)
                    print(f'PASS loopback mode {reason}: finite tail preserved; one release/handoff; LMS index/time continuation; late HTTP/GENA fenced; confirmed gap',flush=True)
                finally:d.close();cli.close();s.close()

if __name__=='__main__':unittest.main()
