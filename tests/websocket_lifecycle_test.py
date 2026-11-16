# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""The room must survive an unsolicited control frame followed by silence."""
import os
import queue
import subprocess
import threading
import time
from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi


def idle_ping():
    owner = Coordinator()
    owner.transport_state = 'PAUSED_PLAYBACK'
    owner.position = dict(Track='1', TrackURI='owned-one', RelTime='00:00:10')
    api = LocalApi(owner, port=0)
    api.websocket_url = f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    fixture = os.environ.get('YENEY_WS_FIXTURE', './speaker-websocket-fixture')
    process = subprocess.Popen([fixture], env=dict(os.environ, YENEY_WS_TEST_PORT=str(api.server.server_port)),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in process.stdout], daemon=True).start()
    try:
        end = time.monotonic()+3
        while time.monotonic() < end:
            fields = lines.get(timeout=3).rstrip('\n').split('\t')
            if float(fields[1]) >= 0:
                break
        else:
            raise AssertionError('room source did not become ready')
        time.sleep(.1)
        assert len(api.handshakes) == 1
        for connection in list(api.connections):
            connection.sendall(b'\x89\x04idle')
        end = time.monotonic()+2
        while time.monotonic() < end:
            fields = lines.get(timeout=3).rstrip('\n').split('\t')
            assert float(fields[1]) >= 0 and not fields[3], fields
        assert len(api.handshakes) == 1, api.handshakes
        print('PASS idle ping followed by silence preserves the room connection and fitted source', flush=True)
    finally:
        process.terminate(); process.wait(5)
        process.stdout.close(); process.stderr.close()
        api.close(); owner.close()



class Source:
    def __init__(self, api):
        self.api = api
        self.rows = queue.Queue()
        self.journal = []
        self.process = subprocess.Popen([os.environ.get('YENEY_WS_FIXTURE', './speaker-websocket-fixture')],
            env=dict(os.environ, YENEY_WS_TEST_PORT=str(api.server.server_port)),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        threading.Thread(target=lambda: [self.rows.put(line.rstrip('\n').split('\t')) for line in self.process.stdout], daemon=True).start()
        threading.Thread(target=lambda: [self.journal.append(line) for line in self.process.stderr], daemon=True).start()
        end = time.monotonic()+3
        while time.monotonic()<end:
            if float(self.rows.get(timeout=3)[1]) >= 0: break
        else: raise AssertionError('source readiness deadline')

    def healthy(self, seconds):
        end = time.monotonic()+seconds
        while time.monotonic()<end:
            row = self.rows.get(timeout=3)
            assert float(row[1]) >= 0 and not row[3], row

    def close(self):
        self.process.terminate(); self.process.wait(5)
        self.process.stdout.close(); self.process.stderr.close()


def controls_and_reconnects():
    import struct
    owner = Coordinator()
    owner.transport_state = 'PAUSED_PLAYBACK'
    owner.position = dict(Track='1', TrackURI='owned-one', RelTime='00:00:10')
    api = LocalApi(owner, port=0)
    api.websocket_url = f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    source = Source(api)
    try:
        for opcode in (9, 10):
            api.control(opcode, b'idle-control')
            source.healthy(1.2)
        assert len(api.handshakes) == 1
        api.ping_before_reply = True
        source.healthy(1.2)
        api.ping_before_reply = False
        assert any(r['opcode']==10 and r['direction']=='received' for r in api.controls)
        print('PASS control frames: idle ping, idle pong and ping with command outstanding preserve the source')
        for label, action in [('close', lambda: api.control(8, struct.pack('!H',1000)+b'fixture close')),
                              ('tcp-end', api.disconnect), ('group-change', lambda: api.regroup('loopback-group-2'))]:
            before = len(api.handshakes)
            started = time.monotonic()
            action()
            source.healthy(1)
            assert len(api.handshakes) == before+1, (label, len(api.handshakes), before)
            assert time.monotonic()-started < 3
            recent = [h for _,h in api.commands if h['command']=='subscribe'][-2:]
            assert len(recent)==2 and all(h['groupId']==api.group for h in recent)
            print(f'PASS {label}: resolve, resubscribe and retain fitted source within 3 s')
        api.subscription_lifetime = .5
        api.broadcast('subscriptionExpired', {})
        before = len([h for _,h in api.commands if h['command']=='subscribe'])
        source.healthy(1.4)
        after = len([h for _,h in api.commands if h['command']=='subscribe'])
        assert after >= before+4, (before,after)
        print('PASS shortened subscription lifetime: expiry renews both namespaces without a source interruption')
        api.subscription_lifetime = 3*24*60*60
        api.reply_delay_once = 1.2
        before = len(api.handshakes)
        source.healthy(2.5)
        assert len(api.handshakes)==before
        print('PASS answer slower than socket timeout does not end connection before its command deadline')
        journal = ''.join(source.journal)
        assert 'control=close code=1000 reason=fixture close' in journal, journal
        assert 'TLS/TCP end' in journal and 'group moved' in journal, journal
    finally:
        source.close(); api.close(); owner.close()


def timers():
    owner = Coordinator()
    api = LocalApi(owner, port=0)
    api.websocket_url = f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    try:
        for mode in ('renew', 'pong', 'missing-pong'):
            api.answer_pings = mode != 'missing-pong'
            before = len(api.commands)
            result = subprocess.run(['./websocket-timer-fixture', f'127.0.0.1:{api.server.server_port}', mode],
                                    capture_output=True,text=True,timeout=5)
            assert result.returncode == 0, result.stdout+result.stderr
            if mode == 'renew':
                assert len([h for _,h in api.commands[before:] if h['command']=='subscribe']) == 4
            assert 'TLS protocol=' in result.stdout
            for cipher in ('ECDHE-RSA-CHACHA20-POLY1305','ECDHE-RSA-AES256-GCM-SHA384','ECDHE-RSA-AES128-GCM-SHA256'):
                assert 'TLS offered_s2='+cipher in result.stdout
            print(result.stdout, end='')
    finally:
        api.close(); owner.close()


def simultaneous_connections():
    import importlib.util
    import statistics
    import tempfile
    from pathlib import Path
    import tarfile
    import json
    owner = Coordinator()
    owner.realtime = True
    owner.transport_state = 'PLAYING'
    owner.position = dict(Track='1',TrackURI='owned-one',RelTime='00:00:10')
    api = LocalApi(owner,port=0)
    api.websocket_url = f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    source = Source(api)
    try:
        source.healthy(.7)
        first_connection = next(iter(api.connections))
        baseline = len([h for _,h in api.commands if h['command']=='getPlaybackStatus'])
        phase_marks = [(1,time.monotonic())]
        source.healthy(1)
        phase_marks.append((2,time.monotonic()))
        check = subprocess.Popen(['./yeney','--position-source-check','--ip',f'127.0.0.1:{api.server.server_port}',
                                  '--seconds','6'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        try:
            end=time.monotonic()+3
            while len(api.connections)<2 and time.monotonic()<end:time.sleep(.01)
            assert len(api.connections)==2
            source.healthy(1)
            module_path=Path(__file__).resolve().parents[1]/'scripts/sonos-local-api-capture.py'
            spec=importlib.util.spec_from_file_location('capture',module_path)
            capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)
            with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR') or '/var/tmp') as directory:
                output=Path(directory)/'capture.tar.gz'
                thread=threading.Thread(target=lambda:capture.capture('127.0.0.1',api.server.server_port,3,output))
                thread.start()
                end=time.monotonic()+2
                while len(api.connections)<3 and time.monotonic()<end:time.sleep(.01)
                assert len(api.connections)==3
                phase_marks.append((3,time.monotonic()))
                errors=[]
                until=time.monotonic()+2
                while time.monotonic()<until:
                    row=source.rows.get(timeout=3)
                    assert float(row[1])>=0 and not row[3],row
                    with owner.lock:
                        truth=owner.position.milliseconds()-(time.monotonic()-float(row[0]))*1000
                    errors.append(float(row[1])-truth)
                thread.join(5)
                assert not thread.is_alive() and output.exists()
                with tarfile.open(output) as archive:
                    counts=json.loads(archive.extractfile('connections.json').read())
                assert counts['held']==0 and counts['peak_tls']==1 and counts['peak_websocket']==1,counts
                assert first_connection in api.connections
                measured_end=time.monotonic()
                for index,(count,begin) in enumerate(phase_marks):
                    finish=phase_marks[index+1][1] if index+1<len(phase_marks) else measured_end
                    delays=[delay for connection,stamp,delay in api.reply_times if connection==id(first_connection) and begin<=stamp<finish]
                    assert delays,(count,api.reply_times)
                    print(f'MEASURE simultaneous={count} first_reply n={len(delays)} median_ms={statistics.median(delays):.6f} max_ms={max(delays):.6f}')
                assert abs(statistics.median(errors))<=10 and max(map(abs,errors))<=30,errors
                print(f'MEASURE simultaneous=3 first_source median_ms={statistics.median(errors):.6f} max_ms={max(map(abs,errors)):.6f} closed=no unanswered=no')
            output=check.communicate(timeout=8)[0]
            assert check.returncode==0,output
            assert 'peak_tls=1' in output and 'peak_websocket=1' in output and 'held_tls=0' in output,output
            assert first_connection in api.connections
            after=len([h for _,h in api.commands if h['command']=='getPlaybackStatus'])
            assert after>baseline
            print('PASS simultaneous connections: room, shared-loop check and capture; first connection stays open and answers within existing millisecond windows')
        finally:
            if check.poll() is None:check.terminate();check.wait(5)
    finally:
        source.close();api.close();owner.close()




def command_deadline():
    owner=Coordinator()
    owner.transport_state='PAUSED_PLAYBACK'
    owner.position=dict(Track='1',TrackURI='owned-one',RelTime='00:00:10')
    api=LocalApi(owner,port=0)
    api.websocket_url=f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    source=Source(api)
    try:
        api.drop_status_answers=True
        end=time.monotonic()+5
        while time.monotonic()<end and not any('no answer for 3 s' in line for line in source.journal):time.sleep(.01)
        text=''.join(source.journal)
        assert 'speaker command failed' in text and 'getPlaybackStatus' in text and 'no answer for 3 s' in text,text
        assert 'socket read' not in text,text
        print('PASS unanswered command fails at its own three-second deadline with its command identity')
    finally:
        source.close();api.close();owner.close()


def capture_control_frames():
    import importlib.util,json,struct,tarfile,tempfile
    from pathlib import Path
    owner=Coordinator();api=LocalApi(owner,port=0)
    api.websocket_url=f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    module_path=Path(__file__).resolve().parents[1]/'scripts/sonos-local-api-capture.py'
    spec=importlib.util.spec_from_file_location('capture',module_path)
    capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)
    timers=[]
    try:
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR') or '/var/tmp') as directory:
            for delay,opcode,body in ((.3,9,b'capture-ping'),(.5,10,b'unsolicited-pong'),(.8,8,struct.pack('!H',1000)+b'normal close')):
                timer=threading.Timer(delay,lambda op=opcode,b=body:api.control(op,b));timers.append(timer);timer.start()
            path=capture.capture('127.0.0.1',api.server.server_port,3,Path(directory)/'controls.tar.gz')
            with tarfile.open(path) as archive:
                rows=[json.loads(line) for line in archive.extractfile('websocket-controls.jsonl').read().splitlines()]
                ending=json.loads(archive.extractfile('connection-ending.json').read())
                connections=json.loads(archive.extractfile('connections.json').read())
            assert {r['control'] for r in rows}=={'ping','pong','close'},rows
            close=next(r for r in rows if r['control']=='close')
            assert close['code']==1000 and close['reason']=='normal close',close
            assert 'websocket close' in ending['ending'] and ending['elapsed']<3,ending
            assert connections['held']==0 and connections['peak_tls']==1,connections
            print('PASS capture records ping/pong/close, code/reason, connection age and exact held/peak counts')
    finally:
        for timer in timers:timer.join()
        api.close();owner.close()




def segmented_diagnostic():
    import re
    owner=Coordinator();owner.realtime=True;owner.transport_state='PLAYING'
    owner.position=dict(Track='1',TrackURI='first-item',RelTime='00:00:10')
    api=LocalApi(owner,port=0);api.websocket_url=f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    def changes():
        time.sleep(.6)
        owner.position=dict(Track='2',TrackURI='second-item',RelTime='00:03:20')
        time.sleep(1)
        owner.position['RelTime']='00:00:30'
    worker=threading.Thread(target=changes)
    try:
        worker.start()
        result=subprocess.run(['./yeney','--position-source-check','--ip',f'127.0.0.1:{api.server.server_port}','--seconds','6'],capture_output=True,text=True,timeout=12)
        worker.join()
        assert result.returncode==0,result.stdout+result.stderr
        assert 'speaker command failed' not in result.stdout,result.stdout
        segments=[line for line in result.stdout.splitlines() if line.startswith('playing segment=')]
        first_phase=[line for line in segments if 'rate=5Hz' in line]
        assert len(first_phase)>=3 and sum('itemId=2 ' in line for line in first_phase)>=2,result.stdout
        for line in segments:
            match=re.search(r'playing_line_slope_ms_s=([^ ]+)',line)
            if match: assert abs(float(match[1])-1000)<100,line
        print('PASS diagnostic fits each item and seek segment separately; no line spans a position reset')
    finally:
        worker.join();api.close();owner.close()


if __name__ == '__main__':
    idle_ping()
    controls_and_reconnects()
    timers()
    simultaneous_connections()
    command_deadline()
    capture_control_frames()
    segmented_diagnostic()
