"""Real isolated SOAP worker against a local simulated Sonos; no hardware."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import html, os, re, subprocess, threading, time
ROOT=Path(__file__).resolve().parents[1]
class Speaker(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_SUBSCRIBE(self):
        self.send_response(200); self.send_header('SID','uuid:timing'); self.send_header('TIMEOUT','Second-3600'); self.send_header('Content-Length','0'); self.end_headers()
    do_UNSUBSCRIBE=do_SUBSCRIBE
    def do_POST(self):
        body=self.rfile.read(int(self.headers['Content-Length'])).decode()
        action=self.headers['SOAPACTION'].split('#')[-1].strip('"')
        srv=self.server
        with srv.lock:
            srv.count[action]=srv.count.get(action,0)+1; count=srv.count[action]
            if action=='GetPositionInfo': srv.requests.append(time.monotonic())
        fields={}
        if action=='GetZoneGroupState':
            coord='member' if srv.mode=='member' else 'study'
            fields={'ZoneGroupState':f'<ZoneGroups><ZoneGroup Coordinator="{coord}"><ZoneGroupMember UUID="study" ZoneName="Study" Location="http://127.0.0.1:{srv.server_port}/"/><ZoneGroupMember UUID="member" ZoneName="MBR" Location="http://127.0.0.1:{srv.server_port}/"/></ZoneGroup></ZoneGroups>'}
        elif action=='GetTransportInfo': fields={'CurrentTransportState':'PAUSED_PLAYBACK' if srv.mode=='paused' or count>1 else 'PLAYING','CurrentTransportStatus':'OK'}
        elif action=='GetPositionInfo':
            if srv.mode=='errors' and count>1:
                self.send_response(500); self.send_header('Content-Length','0'); self.end_headers(); return
            second=123 if count==1 else int(time.monotonic()-srv.started)
            fields={'RelTime':f'0:{second//60:02}:{second%60:02}','TrackDuration':'0:00:00'}
        elif action=='GetMediaInfo': fields={'CurrentURI':'x-sonos-vli:airplay'}
        elif action=='GetVolume': fields={'CurrentVolume':'25'}
        service='ZoneGroupTopology' if action=='GetZoneGroupState' else 'RenderingControl' if action=='GetVolume' else 'AVTransport'
        content=f'<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:{action}Response xmlns:u="urn:schemas-upnp-org:service:{service}:1">'+''.join(f'<{k}>{html.escape(v)}</{k}>' for k,v in fields.items())+f'</u:{action}Response></s:Body></s:Envelope>'
        payload=content.encode(); self.send_response(200); self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
for setting,mode in [(None,'off'),('0','off'),('bad','off'),('', 'off'),('1','member'),('1','paused'),('1','relinquished'),('1','foreign'),('1','mismatch'),('1','active'),('1','errors')]:
    server=ThreadingHTTPServer(('127.0.0.1',0),Speaker);server.mode=mode;server.lock=threading.Lock();server.count={};server.requests=[];server.started=time.monotonic()
    thread=threading.Thread(target=server.serve_forever);thread.start()
    try:
        env=os.environ.copy();env.pop('YENEY_TIMING_PROBE',None)
        if setting is not None: env['YENEY_TIMING_PROBE']=setting
        result=subprocess.run([str(ROOT/'timing-probe-http-test'),str(server.server_port),mode],env=env,capture_output=True,text=True,check=True,timeout=18)
        print(result.stdout,end='')
        baseline=0 if mode=='paused' else 1
        extra=len(server.requests)-baseline
        if mode not in ('active','errors'): assert extra==0,(mode,server.count,result.stdout)
        else:
            assert extra>=(8 if mode=='active' else 3),(server.count,result.stdout)
            if mode=='errors':
                intervals=[b-a for a,b in zip(server.requests[1:],server.requests[2:])]
                assert intervals[0]>=1.9 and intervals[1]>=3.9,intervals
                assert 'SOAP action=GetPositionInfo error=' not in result.stdout
                assert 'errors=3' in result.stdout,result.stdout
            for t in server.requests[1:]:
                assert sum(t-1 < r <= t for r in server.requests[1:])<=8
                assert sum(t-10 < r <= t for r in server.requests[1:])<=30
            lines=[s for s in result.stdout.splitlines() if 'yeney: timing ' in s]
            assert any('reason=periodic' in s for s in lines),result.stdout
            assert any(' final ' in s for s in lines),result.stdout
            for line in lines:
                assert re.search(r'mono=\d+\.\d+ real=\d+\.\d+',line),line
                assert all(k in line for k in ('edges=','uncertainty_ms=','drift_ppm=','lead_p5_ms=','probe_rps=')),line
        if setting in (None,'0'): assert 'yeney: timing ' not in result.stdout
        if setting not in (None,'0','1'): assert result.stdout.count('key=YENEY_TIMING_PROBE invalid=')==1
        print(f'PASS: setting={setting} mode={mode} extra_position_requests={extra}, unchanged cache and rolling limits')
    finally:
        server.shutdown();server.server_close();thread.join()
