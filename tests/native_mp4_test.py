# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""First PCM must precede the unserved encoded tail of immutable tail-moov ALAC."""
import os,pathlib,subprocess,sys,threading,time,struct,zlib
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'third_party/yeney-core/tests'))
from decoders_test import alac_parts,mp4
cookie,packets=alac_parts((ROOT/'third_party/yeney-core/tests/fixtures/alac-16-2.m4a').read_bytes())
full_frames=struct.unpack_from('>I',cookie)[0]
source=mp4(cookie,[packets[0]]*32,full_frames*32,last=True,complete=True)
release=threading.Event();ranges=[]
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_GET(self):
  assert self.path=='/music/13/download'
  value=self.headers.get('Range')
  if value:
   start,end=map(int,value[6:].split('-'));ranges.append((start,end))
   body=source[start:end+1];self.send_response(206)
   self.send_header('Content-Range',f'bytes {start}-{end}/{len(source)}')
  else:body=source;self.send_response(200)
  self.send_header('Content-Length',str(len(body)));self.end_headers()
  try:
   if not value:
    self.wfile.write(body[:65536]);self.wfile.flush()
    release.wait(3);body=body[65536:]
   self.wfile.write(body)
  except (BrokenPipeError,ConnectionResetError):pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler);worker=threading.Thread(target=server.serve_forever);worker.start()
try:
 start=time.monotonic()
 proc=subprocess.Popen([os.environ.get('YENEY_NATIVE_MP4_FIXTURE',str(ROOT/'native-mp4-fixture')),str(server.server_port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 line=proc.stdout.readline();delay=time.monotonic()-start
 assert line.startswith('FIRST '),(line,proc.stderr.read())
 expected=struct.unpack_from('<ii',zlib.decompress((ROOT/'third_party/yeney-core/tests/fixtures/alac-16-2.reference.zlib').read_bytes()))
 assert tuple(map(int,line.split()[1:]))==expected,(line,expected)
 assert delay<.3,delay
 assert ranges and ranges[0][0]>65536,ranges
 assert not release.is_set()
 release.set();stdout,stderr=proc.communicate(timeout=3)
 assert proc.returncode==0,(proc.returncode,stderr)
 print(f'PASS C2 native tail-moov first PCM in {delay*1000:.3f} ms before encoded tail is served; validated range={ranges[0]}',flush=True)
finally:
 release.set();server.shutdown();server.server_close();worker.join()
