"""Generated podcast feed acceptance against the private loopback LMS only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import http.client
import json
import math
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, urlsplit

def run(g):
    cli,wait,mac,sonos,base=(g[k] for k in ('cli','wait','mac','sonos','base'))
    items,packets=g['items'],g['packets']
    body=g['wav'](44100,60)
    audio=base/'podcast.flac'
    subprocess.run(['flac','--silent','--padding=65536','-f','-o',str(audio),'-'],input=body,check=True)
    encoded=audio.read_bytes()
    def encode(seconds,bitrate):
        return subprocess.run(['ffmpeg','-v','error','-i','pipe:0','-codec:a','libmp3lame','-b:a',str(bitrate)+'k',
                               '-write_xing','0','-id3v2_version','0','-f','mp3','pipe:1'],
                              input=g['wav'](44100,seconds),stdout=subprocess.PIPE,check=True).stdout
    # No Xing duration: LMS estimates from first bitrate and Content-Length.
    # Changing bitrate later models an inserted advert without lying in HTTP.
    media_items={0:encoded,1:encode(44,128)+encode(16,160),2:encode(2,128)+encode(58,64),3:encode(60,128)}
    subprocess.run(['make','-C',str(g['ROOT']/'third_party/yeney-core'),'decoder-test'],check=True)
    references={}
    for i in (1,2):
        source=base/f'podcast-{i}.mp3';raw=base/f'podcast-{i}.pcm';source.write_bytes(media_items[i])
        subprocess.run([str(g['ROOT']/'third_party/yeney-core/decoder-test'),'m',str(source),str(raw)],check=True)
        pcm=raw.read_bytes()
        references[i]=b''.join(pcm[n+1:n+4] for n in range(0,len(pcm),4))
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_HEAD(self):self.respond(False)
        def do_GET(self):self.respond(True)
        def respond(self,send):
            if self.path.startswith('/feed'):
                entries=''.join(f'<item><title>Episode {i}</title><guid>{i}</guid><itunes:duration>00:{d//60:02}:{d%60:02}</itunes:duration><enclosure url="{origin}/{i}.{"flac" if i==0 else "mp3"}" type="{"audio/flac" if i==0 else "audio/mpeg"}" length="{len(media_items[i])}"/></item>' for i,d in enumerate((60,64,30,60)))
                data=('<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd"><channel><title>YeneY generated test</title>'+entries+'</channel></rss>').encode()
                self.send_response(200);self.send_header('Content-Type','application/rss+xml');self.send_header('Content-Length',str(len(data)));self.end_headers()
                if send:self.wfile.write(data)
                return
            media=media_items[int(self.path.split('/')[1].split('.')[0])]
            start,end=0,len(media)-1
            ranged=bool(self.headers.get('Range'))
            if ranged:
                first,last=self.headers['Range'].split('=')[1].split('-')
                start=int(first);end=min(int(last) if last else end,end)
            self.send_response(206 if ranged else 200)
            self.send_header('Content-Type','audio/flac' if self.path.endswith('.flac') else 'audio/mpeg')
            self.send_header('Accept-Ranges','bytes')
            self.send_header('Content-Length',str(end-start+1))
            if ranged:self.send_header('Content-Range',f'bytes {start}-{end}/{len(media)}')
            self.end_headers()
            if send:
                try:self.wfile.write(media[start:end+1])
                except (BrokenPipeError,ConnectionResetError):pass
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    origin=f'http://127.0.0.1:{server.server_port}'
    thread=threading.Thread(target=server.serve_forever);thread.start()
    def status():
        return json.loads((base/('stream-pacing-'+b'Study'.hex()+'.json')).read_text())['playback']
    def fetch(uri,offset=None):
        url=urlsplit(uri);conn=http.client.HTTPConnection(url.netloc,timeout=20)
        try:
            conn.request('GET',url.path+'?'+url.query,headers={} if offset is None else {'Range':f'bytes={offset}-'})
            response=conn.getresponse();headers=dict(response.getheaders())
            try:data=response.read()
            except http.client.IncompleteRead as e:data=e.partial
            return response.status,headers,data
        finally:conn.close()
    try:
        cli('podcasts addshow '+quote(origin+'/feed.xml',safe='')+' Generated',mac)
        menu=cli('podcasts items 0 100 want_url:1',mac)
        (base/'podcast-menu.txt').write_text(repr(menu))
        ident=menu[menu.index('name:Generated')-1].split(':',1)[1]
        episodes=cli('podcasts items 0 100 want_url:1 item_id:'+ident,mac)
        (base/'podcast-episodes.txt').write_text(repr(episodes))
        assert 'title:Episode 0' in episodes and 'title:Episode 3' in episodes,episodes
        urls=[f'podcast://{origin}/{i}.'+('flac' if i==0 else 'mp3') for i in range(4)]
        print('PASS real LMS 9.1.1 generated local podcast feed: four itunes:duration cases parsed by native podcast plugin',flush=True)
        for i,url in enumerate(urls):
            cli('stop',mac);cli('playlist clear',mac)
            if i==3:
                # Let the native plugin save a bookmark from real STAT time,
                # then select its generated 'continue' menu entry.
                seed_starts=len(packets('STMs'));seed_decoded=len(packets('STMd'))
                cli('podcasts playlist play item_id:'+ident+'.3',mac)
                wait(lambda:len(packets('STMs'))>seed_starts,5,'bookmark audible start')
                wait(lambda:len(packets('STMd'))>seed_decoded,15,'bookmark source spooled')
                wait(lambda:0<=float(cli('time ?',mac)[-1])<2,5,'bookmark starts at zero')
                wait(lambda:status().get('title')=='Episode 3' and status().get('state')=='PLAYING',5,'bookmark source')
                with sonos.lock:sonos.position['RelTime']='00:00:20'
                wait(lambda:20<=float(cli('time ?',mac)[-1])<22,5,'bookmark source time')
                cli('stop',mac)
                wait(lambda:status().get('state')=='STOPPED',5,'bookmark saved')
                cli('podcasts items 0 100 want_url:1 item_id:'+ident,mac)
                bookmark=cli('podcasts items 0 100 want_url:1 item_id:'+ident+'.3',mac)
                (base/'podcast-bookmark-menu.txt').write_text(repr(bookmark))
                assert 'id:'+ident+'.3.0' in bookmark,bookmark
                print('PASS native podcast saved bookmark: real STAT time 20 s, stop stores progress, continue menu selected',flush=True)
            mark=len((base/'bridge.log').read_text());starts=len(packets('STMs'))
            now=time.monotonic()
            cli('podcasts playlist play item_id:'+ident+'.'+str(i)+('.0' if i==3 else ''),mac)
            wait(lambda:status().get('reason')=='remote-finite' and sonos.transport_state=='PLAYING' and status().get('title')=='Episode '+str(i) and len(items())>0,5,'podcast immediate relay')
            delay=time.monotonic()-now
            fields=cli('status 0 1 tags:udV',mac)
            (base/f'podcast-{i}-status.txt').write_text(repr(fields))
            wait(lambda:'queue item-end' in (base/'bridge.log').read_text()[mark:],15,'podcast decoded')
            time.sleep(.3)
            current=items()
            chunks=[fetch(e['uri']) for e in current]
            declared=int(chunks[0][1]['Content-Length'])
            frames=int.from_bytes(chunks[0][2][40:44],'little')//6
            assert declared==44+frames*6
            duration=float([f.split(':',1)[1] for f in fields if f.startswith('duration:')][-1])
            assert frames==math.ceil((duration-(20 if i==3 else 0))*44100)+5*44100
            for entry,chunk in zip(current,chunks):
                declared_frames=int.from_bytes(chunk[2][40:44],'little')//6
                assert int(chunk[1]['Content-Length'])==44+declared_frames*6==int(entry['resource']['size'])
                h,m,s=map(float,entry['resource']['duration'].split(':'))
                assert abs(h*3600+m*60+s-declared_frames/44100)<.0011
            print(f'PODCAST case={i} startup={delay:.3f}s declared_frames={frames} items={len(current)} LMS={fields}',flush=True)
            assert delay<5
            joined=b''.join(c[2][44:] for c in chunks)
            # wav fixture is 16-bit; relay expands to 24 bits independently.
            expected=b''.join(b'\0'+body[n:n+2] for n in range(44,len(body),2))
            if i==3:
                assert 39.8*44100*6 <= len(joined) <= 40.2*44100*6,(i,len(joined))
                assert abs(frames/44100-45)<1,(frames,fields)
            else:
                if i in references:expected=references[i]
                assert joined==expected,(i,len(joined),len(expected))
            if i==1:assert 63<duration<65,(duration,fields)
            if i==2:
                assert 30<duration<33 and len(current)>1,(duration,len(current))
                assert 'continuation inserted' in (base/'bridge.log').read_text()[mark:]
            real=44+len(chunks[-1][2])-44
            code,headers,data=fetch(current[-1]['uri'],real)
            if real<int(chunks[-1][1]['Content-Length']):
                assert code==206 and not data
                assert headers['Content-Range']==f'bytes {real}-{int(chunks[-1][1]["Content-Length"])-1}/{chunks[-1][1]["Content-Length"]}'
            offset=declared//2
            code,headers,data=fetch(current[0]['uri'],offset)
            assert code==206 and data==chunks[0][2][offset:]
            wait(lambda:len(packets('STMs'))==starts+1,3,'one episode STMs')
            print(f'PASS podcast case {i}: WAV/Content-Length/DIDL declared frames agree; '+('sample-exact decoded source and continuations' if i<3 else 'native MP3 resume from 20 s, approximately 40 s decoded')+'; Range at actual EOF and midpoint',flush=True)
            if i==0:
                with sonos.lock:sonos.position['RelTime']='00:00:30'
                wait(lambda:float(cli('time ?',mac)[-1])>=29,5,'app seek LMS time')
                print('PASS app-equivalent seek: LMS time='+cli('time ?',mac)[-1]+'; no extra CLI seek command',flush=True)
            for segment in current[1:]:
                with sonos.lock:
                    sonos.position=dict(Track=str(sonos.items.index(segment)+1),TrackURI=segment['uri'],RelTime='00:00:00')
                    sonos.transport_state='PLAYING'
                wait(lambda:status().get('current_owned_item')==segment['uri'],3,'continuation speaker clock')
            assert len(packets('STMs'))==starts+1,'continuation created another LMS track start'
            ends=len(packets('STMu'))
            end_seconds=(len(chunks[-1][2])-44)//6//44100
            with sonos.lock:
                sonos.position['RelTime']=f'{end_seconds//3600:02}:{end_seconds//60%60:02}:{end_seconds%60:02}'
                sonos.transport_state='STOPPED'
            wait(lambda:len(packets('STMu'))>ends and cli('mode ?',mac)[-1]=='stop',5,'real EOF reaches LMS')
            print(f'PASS podcast case {i}: short real EOF reaches LMS STMu without waiting for declared end',flush=True)
        commands=[r['request'] for r in g['proxy'].records if not (' status ' in r['request'] or r['request'].endswith('?'))]
        assert not commands,commands
        cli('stop',mac)
        radio=g['Remote']((g['ROOT']/'third_party/yeney-core/tests/fixtures/gapless-0.mp3').read_bytes(),kind='mp3',endless=True)
        try:
            cli('playlist clear',mac);cli('playlist add '+radio.url,mac)
            additions=len(sonos.actions('AddURIToQueue'));cli('play',mac)
            wait(lambda:status().get('path')=='stream' and sonos.transport_state=='PLAYING',5,'endless radio path')
            assert status()['reason']=='remote-source' and len(sonos.actions('AddURIToQueue'))==additions
            print('PASS real LMS local endless radio: stream path remote-source, no queue mutation',flush=True)
            cli('stop',mac)
        finally:radio.close()
    finally:
        server.shutdown();server.server_close();thread.join()
