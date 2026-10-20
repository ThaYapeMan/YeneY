"""Local Sonos API probe against an actual self-signed TLS WebSocket server."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import base64
import hashlib
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import json
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from yeney_queue_ws import KEY,PROTOCOL,WebSocket,Sampler,analyze


class Server(ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self,cert,key,kind='present'):
        self.kind=kind;self.commands=[];self.errors=[]
        super().__init__(('127.0.0.1',0),Handler)
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
        self.socket=context.wrap_socket(self.socket,server_side=True)
        self.worker=threading.Thread(target=self.serve_forever,name='device-fake-wss');self.worker.start()
    def close(self):self.shutdown();self.server_close();self.worker.join()


class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def do_GET(self):
        try:
            assert self.headers.get('X-Sonos-Api-Key')==KEY
            if self.path=='/api/v1/players/local/info':
                body=json.dumps(dict(householdId='household',groupId='group')).encode()
                self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body);return
            assert self.path=='/websocket/api'
            assert self.headers['Sec-WebSocket-Protocol']==PROTOCOL
            accept=base64.b64encode(hashlib.sha1((self.headers['Sec-WebSocket-Key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            self.send_response(101);self.send_header('Upgrade','websocket');self.send_header('Connection','Upgrade')
            self.send_header('Sec-WebSocket-Protocol',PROTOCOL);self.send_header('Sec-WebSocket-Accept',accept);self.end_headers()
            self.connection.settimeout(3)
            count=0
            while True:
                prefix=self.rfile.read(2)
                if not prefix:break
                a,b=prefix;assert a==0x81 and b&128
                n=b&127
                if n==126:n=struct.unpack('!H',self.rfile.read(2))[0]
                mask=self.rfile.read(4);body=self.rfile.read(n)
                headers,args=json.loads(bytes(v^mask[i%4] for i,v in enumerate(body)))
                assert args=={} and headers['namespace']=='playback:1' and headers['name']=='getPlaybackStatus'
                assert headers['householdId']=='household' and headers['groupId']=='group'
                self.server.commands.append(headers);count+=1
                value=count*201+13 if self.server.kind=='present' else count//5*1000
                reply=dict(playbackState='PLAYBACK_STATE_PLAYING',itemId='one',positionMillis=value,previousPositionMillis=max(0,value-200))
                response=dict(requestId=headers['requestId'],namespace='playback:1',name='getPlaybackStatus')
                if self.server.kind=='unavailable':reply.pop('positionMillis')
                if self.server.kind=='inflated':
                    response['type']='error';reply.update(positionMillis=2**63,previousPositionMillis=2**63-1000,errorCode='ERROR_RESOURCE_GONE')
                payload=json.dumps([response,reply]).encode()
                self.wfile.write(bytes([0x81,126])+struct.pack('!H',len(payload))+payload);self.wfile.flush()
        except (ConnectionError,socket.timeout,ssl.SSLError):pass
        except Exception as e:self.server.errors.append(repr(e))


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='queue-device-tls-');root=Path(cls.tmp.name)
        cls.cert=root/'cert.pem';cls.key=root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(cls.key),'-out',str(cls.cert),
                        '-subj','/CN=localhost','-days','1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_tls_shapes(self):
        for kind in ('present','whole','unavailable','inflated'):
            with self.subTest(kind=kind):
                server=Server(self.cert,self.key,kind)
                try:
                    client=WebSocket('127.0.0.1',server.server_port)
                    try:
                        if kind=='inflated':
                            with self.assertRaisesRegex(ValueError,'getPlaybackStatus error'):client.status()
                        else:
                            row=client.status();self.assertLessEqual(row['send'],row['receive'])
                            self.assertEqual(row['itemId'],'one')
                            self.assertEqual(row['positionMillis'] is None,kind=='unavailable')
                    finally:client.close()
                    self.assertEqual(len(server.commands),1)
                finally:server.close()
                self.assertFalse(server.errors,server.errors)
        print('PASS queue device WebSocket TLS: self-signed info/upgrade, public key/subprotocol, read-only correlated array messages, absent and inflated-after-error values')
    def test_sampler_cadence_and_shutdown(self):
        server=Server(self.cert,self.key)
        try:
            with tempfile.TemporaryDirectory() as directory:
                sampler=Sampler('127.0.0.1',directory,lambda:dict(mode='queue',scenario='album'),server.server_port)
                sampler.start();time.sleep(1.25);sampler.close()
                self.assertGreaterEqual(len(sampler.rows),5)
                self.assertLessEqual(len(sampler.rows),8)
                self.assertFalse(sampler.worker.is_alive())
                self.assertTrue((Path(directory)/'websocket.jsonl').exists())
        finally:server.close()
        print('PASS queue device WebSocket sampler: 5 Hz read-only polling and bounded thread/socket shutdown')
    def test_statistics(self):
        for whole in (False,True):
            rows=[]
            for mode in ('queue','stream'):
                for i in range(100):
                    midpoint=100+i*.2
                    value=(i//5)*1000 if whole else i*200+17
                    rows.append(dict(mode=mode,scenario='album',send=midpoint-.005,receive=midpoint+.005,
                                     midpoint=midpoint,itemId='item',positionMillis=value,previousPositionMillis=max(0,value-200),playbackState='PLAYBACK_STATE_PLAYING'))
            report=analyze(rows,[dict(scenario='album',confirmed_monotonic=100,rel_time=0)])
            for mode in ('queue','stream'):
                self.assertEqual(report[mode]['verdict'],'whole seconds' if whole else 'sub-second')
                self.assertAlmostEqual(report[mode]['round_trip_median_ms'],10)
                self.assertTrue(report[mode]['fits'])
            self.assertAlmostEqual(report['queue']['item_starts'][0]['difference_ms'],0 if whole else 17)
        missing_id=dict(rows[0],itemId=None)
        report=analyze([missing_id],[])
        self.assertTrue(report['queue']['position_present'])
        self.assertEqual(report['queue']['verdict'],'unavailable')
        self.assertIn('position present but itemId unavailable',report['queue']['reasons'])
        report=analyze([dict(mode='queue',error='resource gone',positionMillis=2**63)],[])
        self.assertEqual(report['queue']['verdict'],'unavailable')
        self.assertFalse(report['queue']['position_present'])
        print('PASS queue device WebSocket metrics: per-mode presence, whole-second share, distinct values, RTT, per-item fit/start residuals; errors never fabricate precision')
    def test_unavailable_is_nonfatal(self):
        server=Server(self.cert,self.key,'inflated')
        try:
            with tempfile.TemporaryDirectory() as directory:
                sampler=Sampler('127.0.0.1',directory,lambda:dict(mode='stream',scenario='reference'),server.server_port)
                sampler.start();sampler.worker.join(4);sampler.close()
                report=analyze(sampler.rows,[])
                self.assertEqual(report['stream']['verdict'],'unavailable')
                self.assertIn('getPlaybackStatus error',report['stream']['reasons'][0])
        finally:server.close()
        print('PASS queue device WebSocket failure: unavailable reason archived without a device-suite failure')


if __name__=='__main__':unittest.main(verbosity=2)
