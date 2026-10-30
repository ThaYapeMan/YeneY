"""Generated podcast feed acceptance against the private loopback LMS only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import http.client
import json
import math
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, unquote, urlsplit
from xml.sax.saxutils import escape

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
    media_items={0:encoded,1:encode(44,128)+encode(16,160),2:encode(80,128)+encode(120,64),3:encode(200,128)}
    subprocess.run(['make','-C',str(g['ROOT']/'third_party/yeney-core'),'decoder-test'],check=True)
    references={}
    for i in (1,2):
        source=base/f'podcast-{i}.mp3';raw=base/f'podcast-{i}.pcm';source.write_bytes(media_items[i])
        subprocess.run([str(g['ROOT']/'third_party/yeney-core/decoder-test'),'m',str(source),str(raw)],check=True)
        pcm=raw.read_bytes()
        references[i]=b''.join(pcm[n+1:n+4] for n in range(0,len(pcm),4))
    source_ranges=[]
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_HEAD(self):self.respond(False)
        def do_GET(self):self.respond(True)
        def respond(self,send):
            if self.path.startswith('/feed'):
                feed_query=urlsplit(self.path).query
                feed_name=unquote(feed_query) or 'YeneY generated test'
                suffix='?feed='+quote(feed_name,safe='') if feed_query else ''
                guid_prefix=escape(feed_name+':') if feed_query else ''
                entries=''.join(f'<item><title>Episode {i}</title><guid>{guid_prefix}{i}</guid><itunes:duration>00:{d//60:02}:{d%60:02}</itunes:duration><enclosure url="{origin}/{i}.{"flac" if i==0 else "mp3"}{suffix}" type="{"audio/flac" if i==0 else "audio/mpeg"}" length="{len(media_items[i])}"/></item>' for i,d in enumerate((60,64,140,200)))
                data=('<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd"><channel><title>'+escape(feed_name)+'</title><itunes:author>Fixture Author</itunes:author>'+entries+'</channel></rss>').encode()
                self.send_response(200);self.send_header('Content-Type','application/rss+xml');self.send_header('Content-Length',str(len(data)));self.end_headers()
                if send:self.wfile.write(data)
                return
            media_path=urlsplit(self.path).path
            media=media_items[int(media_path.split('/')[1].split('.')[0])]
            start,end=0,len(media)-1
            ranged=bool(self.headers.get('Range'))
            if ranged:
                first,last=self.headers['Range'].split('=')[1].split('-')
                start=int(first);end=min(int(last) if last else end,end)
            if send:source_ranges.append((int(media_path.split('/')[1].split('.')[0]),start,end))
            self.send_response(206 if ranged else 200)
            self.send_header('Content-Type','audio/flac' if media_path.endswith('.flac') else 'audio/mpeg')
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
    def feed_id():
        rows=[]
        for field in cli('podcasts items 0 100 want_url:1',mac):
            key,sep,value=field.partition(':')
            if key=='id':rows.append({'id':value})
            elif rows and sep:rows[-1][key]=value
        matches=[row['id'] for row in rows if row.get('url')==origin+'/feed.xml']
        assert len(matches)==1,rows
        return matches[0]
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
        title_lookup=subprocess.run([str(g['ROOT']/'queue-source-test'),'127.0.0.1',mac,
                                     'Episode 3 by Fixture Author from Generated'],capture_output=True,text=True,check=True)
        assert title_lookup.stdout.strip()=='Episode 3',title_lookup.stdout
        print('PASS native podcast plain-title lookup: actual read-only menu resolves stored formatted TITLE with absent artist/album fields',flush=True)
        print('PASS real LMS 9.1.1 generated local podcast feed: four itunes:duration cases parsed by native podcast plugin',flush=True)
        if g['os'].environ.get('YENEY_PODCAST_METADATA_MATRIX','1')=='1':
            from podcast_metadata_test import native_matrix
            native_matrix(cli,mac,origin,base,g['ROOT'])
        for i,url in enumerate(urls):
            ident=feed_id()
            cli('stop',mac);cli('playlist clear',mac)
            if i==3:
                # Let the native plugin save a bookmark from real STAT time,
                # then select its generated 'continue' menu entry.
                seed_starts=len(packets('STMs'));seed_decoded=len(packets('STMd'))
                cli('podcasts playlist play item_id:'+ident+'.3',mac)
                wait(lambda:len(packets('STMs'))>seed_starts,5,'bookmark audible start')
                wait(lambda:'queue item-end' in (base/'bridge.log').read_text(),15,'bookmark source spooled')
                wait(lambda:0<=float(cli('time ?',mac)[-1])<2,5,'bookmark starts at zero')
                wait(lambda:status().get('title')=='Episode 3' and status().get('state')=='PLAYING',5,'bookmark source')
                with sonos.lock:sonos.position['RelTime']='00:01:40'
                wait(lambda:100<=float(cli('time ?',mac)[-1])<102,5,'bookmark source time')
                cli('stop',mac)
                wait(lambda:status().get('state')=='STOPPED',5,'bookmark saved')
                ident=feed_id()
                bookmark=[]
                def bookmark_saved():
                    nonlocal bookmark
                    cli('podcasts items 0 100 want_url:1 item_id:'+ident,mac)
                    bookmark=cli('podcasts items 0 100 want_url:1 item_id:'+ident+'.3',mac)
                    return 'id:'+ident+'.3.0' in bookmark
                wait(bookmark_saved,5,'native bookmark menu published')
                (base/'podcast-bookmark-menu.txt').write_text(repr(bookmark))
                print('PASS native podcast saved bookmark: real STAT time 100 s, stop stores progress, continue menu selected',flush=True)
            mark=len((base/'bridge.log').read_text());starts=len(packets('STMs'));decoded_reports=len(packets('STMd'))
            now=time.monotonic();range_mark=len(source_ranges)
            cli('podcasts playlist play item_id:'+ident+'.'+str(i)+('.0' if i==3 else ''),mac)
            wait(lambda:status().get('reason')=='remote-finite' and sonos.transport_state=='PLAYING' and status().get('title')=='Episode '+str(i) and len(items())>0,5,'podcast immediate relay')
            delay=time.monotonic()-now
            if i==3:
                # Pause while PCM production is still partial. Encoded bytes
                # have independent, genuinely disk-backed receive capacity.
                from queue_sonos_fake import FiniteDeviceAudio
                model=FiniteDeviceAudio(sonos,speed=1,progressive=True);sonos.device_audio=model;model.start()
                wait(lambda:len(model.audio)>3.2*44100*6,10,'partial producer actual speaker audio')
                assert 'queue item-end' not in (base/'bridge.log').read_text()[mark:]
                kept=[e['uri'] for e in items()];adds=len(sonos.actions('AddURIToQueue'))
                with sonos.lock:model.pause()
                paused_frame=model.frame
                wait(lambda:cli('mode ?',mac)[-1]=='pause',5,'partial producer app pause')
                paused=time.monotonic()
                assert abs(float(cli('time ?',mac)[-1])-(100+model.frame//44100))<1.25,'partial pause position differs from speaker'
                print('PARTIAL PAUSE 120 real seconds: native status='+repr(cli('status 0 60000 tags:alKudV',mac)),flush=True)
                for _ in range(12):time.sleep(10)
                assert model.frame==paused_frame and [e['uri'] for e in items()]==kept
                assert 'queue item-end' not in (base/'bridge.log').read_text()[mark:]
                assert not any(r.get('command')=='q' and r['when']>=paused for r in g['tap'].records),'native buffer-full pause must not discard a disk-backed producer'
                with sonos.lock:model.resume()
                wait(lambda:cli('mode ?',mac)[-1]=='play',5,'partial producer app resume')
                assert abs(float(cli('time ?',mac)[-1])-(100+model.frame//44100))<1.25
                model.speed=5
                assert len(sonos.actions('AddURIToQueue'))==adds
                assert len(packets('STMs'))==starts+1,'partial episode restarted'
                print('PASS native LMS 120 s partial-production pause: real encoded disk capacity prevents buffer-full q; same queue URI, same producer/decoder and LMS track, ordinary p/u',flush=True)
            fields=cli('status 0 1 tags:udV',mac)
            (base/f'podcast-{i}-status.txt').write_text(repr(fields))
            wait(lambda:'queue item-end' in (base/'bridge.log').read_text()[mark:],60,'podcast decoded')
            if i==2:wait(lambda:len(items())==2,5,'exact continuation confirmed in queue')
            current=items()
            assert len(packets('STMd'))==decoded_reports,'finite episode STMd before playback end'
            chunks=[fetch(e['uri']) for e in current]
            declared=int(chunks[0][1]['Content-Length'])
            frames=int.from_bytes(chunks[0][2][40:44],'little')//6
            assert declared==44+frames*6
            duration=float([f.split(':',1)[1] for f in fields if f.startswith('duration:')][-1])
            assert frames==math.ceil(duration*44100)+5*44100
            for entry,chunk in zip(current,chunks):
                declared_frames=int.from_bytes(chunk[2][40:44],'little')//6
                assert int(chunk[1]['Content-Length'])==44+declared_frames*6==int(entry['resource']['size'])
                h,m,s=map(float,entry['resource']['duration'].split(':'))
                assert abs(h*3600+m*60+s-declared_frames/44100)<.0011
            if i==2:
                assert len(current)==2,len(current)
                text=(base/'bridge.log').read_text()[mark:]
                assert 'decision=exact kind=exact' in text,text[-10000:]
                print('PASS native LMS continuation: exactly one exact remainder for the underestimated MP3 episode',flush=True)
            print(f'PODCAST case={i} startup={delay:.3f}s declared_frames={frames} items={len(current)} LMS={fields}',flush=True)
            assert delay<5
            joined=b''.join(c[2][44:] for c in chunks)
            # wav fixture is 16-bit; relay expands to 24 bits independently.
            expected=b''.join(b'\0'+body[n:n+2] for n in range(44,len(body),2))
            if i==3:
                assert 99.8*44100*6 <= len(joined) <= 100.2*44100*6,(i,len(joined))
                ranges=[r for r in source_ranges[range_mark:] if r[0]==3 and r[1]>0 and r[2]==len(media_items[3])-1]
                assert ranges,source_ranges[range_mark:]
                _,begin,end=ranges[-1]
                source=base/'continued-source.mp3';raw=base/'continued-source.pcm'
                source.write_bytes(media_items[3][begin:end+1])
                subprocess.run([str(g['ROOT']/'third_party/yeney-core/decoder-test'),'m',str(source),str(raw)],check=True)
                pcm=raw.read_bytes();expected=b''.join(pcm[n+1:n+4] for n in range(0,len(pcm),4))
                assert joined==expected,(len(joined),len(expected))
                assert abs(frames/44100-205)<1,(frames,fields)
            else:
                if i in references:expected=references[i]
                assert joined==expected,(i,len(joined),len(expected))
            if i==1:assert 63<duration<65,(duration,fields)
            if i==2:
                assert 139<duration<142 and len(current)>1,(duration,len(current))
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
            print(f'PASS podcast case {i}: WAV/Content-Length/DIDL declared frames agree; '+('sample-exact decoded source and continuations' if i<3 else 'native MP3 resume from 100 s, approximately 100 s decoded')+'; Range at actual EOF and midpoint',flush=True)
            if i==0:
                with sonos.lock:sonos.position['RelTime']='00:00:30'
                wait(lambda:float(cli('time ?',mac)[-1])>=29,5,'app seek LMS time')
                print('PASS app-equivalent seek: LMS time='+cli('time ?',mac)[-1]+'; no extra CLI seek command',flush=True)
            if i in (2,3):
                from queue_sonos_fake import FiniteDeviceAudio
                if i==2:
                    model=FiniteDeviceAudio(sonos,speed=5)
                    sonos.device_audio=model
                    with sonos.lock:
                        sonos.position=dict(Track=str(sonos.items.index(current[0])+1),TrackURI=current[0]['uri'],RelTime='00:00:00')
                        sonos.transport_state='PLAYING'
                        model.start()
                if i==3:
                    wait(lambda:len(model.audio)>44100*6*3,10,'continued episode audio')
                    # Mid-item seek/rebuffer notifications carry no end meaning.
                    ends=len(packets('STMu'));model.speed=1
                    with sonos.lock:model.transitioning()
                    time.sleep(1)
                    assert len(packets('STMu'))==ends
                    with sonos.lock:sonos.transport_state='PLAYING';model.stamp=time.monotonic()
                    time.sleep(3.2)
                    with sonos.lock:model.pause()
                    wait(lambda:cli('mode ?',mac)[-1]=='pause',5,'native podcast app pause')
                    paused_frame=model.frame;kept=[e['uri'] for e in items()]
                    g['tap'].report_full_paused()
                    print('STIMULUS native LMS remote pause with full input buffer (STAT bufferFullness=bufferSize)',flush=True)
                    wait(lambda:'pause flush retained:' in (base/'bridge.log').read_text()[mark:],5,'native remote full-buffer pause flush retained')
                    assert abs(float(cli('time ?',mac)[-1])-(100+model.frame//44100))<1.25,'LMS pause position differs from speaker'
                    print('PAUSE native LMS status='+repr(cli('status 0 60000 tags:alKudV',mac)),flush=True)
                    print('PAUSE full-buffer flush/cached echo check; frame='+str(paused_frame),flush=True)
                    time.sleep(2)
                    assert model.frame==paused_frame and [e['uri'] for e in items()]==kept
                    with sonos.lock:model.resume()
                    wait(lambda:cli('mode ?',mac)[-1]=='play',5,'native podcast app resume')
                    wait(lambda:'resume echo adopted:' in (base/'bridge.log').read_text()[mark:] and len(packets('STMs'))>=starts+2,5,'native cached start acknowledged')
                    wait(lambda:abs(float(cli('time ?',mac)[-1])-(100+model.frame//44100))<1.25,5,'native LMS cached clock acknowledged')
                    print('RESUME native LMS status='+repr(cli('status 0 60000 tags:alKudV',mac)),flush=True)
                    model.speed=5
                wait(lambda:sonos.transport_state=='STOPPED',65,'device consumed real episode tail')
                wait(lambda:cli('mode ?',mac)[-1]=='stop',5,'audible completion in LMS')
                assert not sonos.errors,sonos.errors
                if i==2:assert model.prefetch and 6<=model.prefetch[0][1]<=7,model.prefetch
                assert bytes(model.audio)==joined,(i,len(model.audio),len(joined))
                assert model.audio[-44100*6:]==joined[-44100*6:]
                assert len(packets('STMs'))==starts+1+(1 if i==3 and 'resume echo adopted:' in (base/'bridge.log').read_text()[mark:] else 0)
                print(f'PASS field replay case={i}: every frame including final second reaches device; declared-boundary stop, 6.5 s prefetch; one LMS episode',flush=True)
                if i==3:print('PASS native LMS full-buffer pause flush: immutable queue/spool retained, GET 0 probe plus 206 paused Range, sample-exact complete output; TRANSITIONING mid-item never ends',flush=True)
                model.close();del sonos.device_audio
            else:
                ends=len(packets('STMu'))
                end_seconds=(len(chunks[-1][2])-44)//6//44100
                with sonos.lock:
                    sonos.position['RelTime']=f'{end_seconds//3600:02}:{end_seconds//60%60:02}:{end_seconds%60:02}'
                    sonos.transport_state='STOPPED'
                wait(lambda:len(packets('STMu'))>ends and cli('mode ?',mac)[-1]=='stop',5,'real EOF reaches LMS')
            print(f'PASS podcast case {i}: real EOF reaches LMS only after every produced frame played',flush=True)
        cli('stop',mac)
        repeated=len(packets('STMs'))
        cli('podcasts playlist play item_id:'+ident+'.3',mac)
        wait(lambda:len(packets('STMs'))>repeated and status().get('title')=='Episode 3',5,'restart plain episode title')
        print('RESTART native LMS status='+repr(cli('status 0 60000 tags:alKudV',mac)),flush=True)
        print('PASS same episode fresh/continue/restart: identical plain title Episode 3',flush=True)
        for record in g['proxy'].records:
            if 'status 0 60000 tags:alKudV' in record['request']:
                print('BRIDGE STATUS '+record['reply'],flush=True)
        cli('stop',mac)
        commands=[r['request'] for r in g['proxy'].records if any(' '+word in r['request'] for word in ('stop','pause ','playlist ')) and not r['request'].endswith('?')]
        assert commands==[mac+' pause 1',mac+' pause 0']*2,commands
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
