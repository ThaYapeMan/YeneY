# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Header-derived exact length precedes EOF; mismatches retain PCM and EOF publication."""
import os,pathlib,subprocess,threading,struct,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
ROOT=pathlib.Path(__file__).resolve().parents[1]
frames=10000
pairs=[(((i*137+17)%65536)-32768,((i*59)%65536)-32768) for i in range(frames)]
expected=1469598103934665603
for left,right in pairs:
 for byte in struct.pack('<ii',left*65536,right*65536):expected=((expected^byte)*1099511628211)&((1<<64)-1)
def chunk(tag,body,big=False):return tag+struct.pack('>I' if big else '<I',len(body))+body+(b'\0' if len(body)&1 else b'')
failures=[]
for kind in ('wav','riff-mismatch','data-mismatch','aiff','comm-mismatch'):
 big=kind in ('aiff','comm-mismatch');pcm=b''.join(struct.pack('>hh' if big else '<hh',*pair) for pair in pairs)
 if big:
  comm=struct.pack('>HIH',2,frames+(kind=='comm-mismatch'),16)+bytes.fromhex('400eac44000000000000')
  body=b'AIFF'+chunk(b'COMM',comm,True)+chunk(b'SSND',struct.pack('>II',3,0)+b'pad'+pcm,True)
  body=b'FORM'+struct.pack('>I',len(body))+body
 else:
  fmt=struct.pack('<HHIIHH',1,2,44100,176400,4,16)
  data=chunk(b'data',pcm)
  if kind=='data-mismatch':data=data[:4]+struct.pack('<I',len(pcm)+4000)+data[8:]
  body=b'WAVE'+chunk(b'fmt ',fmt)+chunk(b'JUNK',b'odd')+data
  if kind!='data-mismatch':body+=chunk(b'LIST',b'metadata')
  body=b'RIFF'+struct.pack('<I',len(body)+(4000 if kind=='riff-mismatch' else 0))+body
 release=threading.Event()
 class Handler(BaseHTTPRequestHandler):
  def log_message(self,*_):pass
  def do_GET(self):
   assert self.path=='/music/15/download'
   self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
   try:self.wfile.write(body[:1024]);self.wfile.flush();release.wait(3);self.wfile.write(body[1024:])
   except (BrokenPipeError,ConnectionResetError):pass
 server=ThreadingHTTPServer(('127.0.0.1',0),Handler);worker=threading.Thread(target=server.serve_forever);worker.start()
 try:
  start=time.monotonic();proc=subprocess.Popen([os.environ.get('YENEY_NATIVE_PCM_FIXTURE',str(ROOT/'native-pcm-fixture')),str(server.server_port),'0' if big else '1'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  early=proc.stdout.readline().strip();delay=time.monotonic()-start
  assert early.startswith('EARLY '),(early,proc.stderr.read())
  assert delay<.3 and not release.is_set(),(kind,delay)
  release.set();text,error=proc.communicate(timeout=4)
  assert proc.returncode==0,(kind,proc.returncode,error)
  assert text.strip()==f'FINAL {frames} {expected}',(kind,text,expected)
  wanted=str(frames) if kind in ('wav','aiff') else 'unknown'
  if early!='EARLY '+wanted:failures.append((kind,early,wanted))
  print(f'C3 {kind} before_EOF={early} expected={wanted} PCM=identical delay_ms={delay*1000:.3f}',flush=True)
 finally:release.set();server.shutdown();server.server_close();worker.join()
assert not failures,failures
print('PASS C3 WAV/AIFF exact header count before EOF; inconsistent headers fall back; identical PCM',flush=True)
