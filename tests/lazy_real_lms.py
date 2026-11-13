# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Deferred lazy publication/activation against private loopback LMS 9.1.1."""
import http.client
import time
import socket
import threading
from urllib.parse import urlsplit


def run(context):
    cli, mac, wait = (context[k] for k in ('cli', 'mac', 'wait'))
    items, select_item, packets = (context[k] for k in ('items', 'select_item', 'packets'))
    tracks, proxy, base = (context[k] for k in ('tracks', 'proxy', 'base'))

    def load(ids):
        cli('stop', mac)
        wait(lambda:not items(), 10, 'owned release')
        cli('playlistcontrol cmd:load track_id:'+','.join(ids), mac)
        wait(lambda:bool(items()), 15, 'publication')
        select_item(0)

    def fetch(item, upper=False):
        url=urlsplit(item['uri'])
        conn=http.client.HTTPConnection(url.netloc, timeout=10)
        start=time.monotonic()
        try:
            conn.request('GET',url.path+('?' + url.query if url.query else ''))
            response=conn.getresponse()
            elapsed=time.monotonic()-start
            headers=response.getheaders()
            try:body=response.read()
            except http.client.IncompleteRead as error:
                if not upper:raise
                body=error.partial
            assert response.status==200 and body[:4]==b'RIFF'
            assert len(body)<=int(dict(headers)['Content-Length']) if upper else len(body)==int(dict(headers)['Content-Length'])
            return headers,body,elapsed
        finally:conn.close()

    fillers=context['window_ids'][:2]
    targets=[('local', context['window_ids'][2])]
    for name in ('native-flac-96000-2.flac','native-lengthalac-96000-2.m4a'):
        targets.append((name,next(t['id'] for t in tracks if t.get('url','').endswith('/'+name))))
    for label,target in targets:
        load([target])
        natural=fetch(items()[0])
        load(fillers+[target]+context['window_ids'][3:5])
        wait(lambda:len(items())>=4, 10, 'three upcoming parents')
        lazy=items()[2]
        assert 'size' not in lazy['resource'],lazy
        before=len(proxy.records)
        # HEAD, a header-only GET, and invalid syntax must never select LMS.
        url=urlsplit(lazy['uri'])
        for method,headers in [('HEAD',{}),('GET',{'Range':'bytes=0-43'}),('GET',{'Range':'bytes=broken'})]:
            conn=http.client.HTTPConnection(url.netloc,timeout=5)
            try:
                conn.request(method,url.path+('?' + url.query if url.query else ''),headers=headers)
                response=conn.getresponse();response.read()
                assert response.status in (416,503)
            finally:conn.close()
        assert not [r for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        select_item(2)
        deferred=fetch(lazy)
        assert deferred[:2]==natural[:2],label
        assert deferred[2]<3,(label,deferred[2])
        time.sleep(.5)
        selections=[r['request'] for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
        assert selections==[mac+' playlist index 2'],selections
        log=(base/'bridge.log').read_text()
        assert 'queue lazy activated lms_index=2 lms_commands=1' in log
        assert 'queue lazy bound lms_index=2 lms_commands=1' in log
        print(f'PASS real LMS lazy {label}: three upcoming parents; probes select zero; two-ahead GET selects once; headers/audio byte-identical to natural; headers_s={deferred[2]:.3f} deadline_s=3; no echo command',flush=True)
    load(context['window_ids'])
    cli('playlist shuffle 1',mac)
    def shuffled_window():
        fields=cli('status 0 100 tags:u',mac)
        titles=[field[6:] for field in fields if field.startswith('title:')]
        index=int(next(field.split(':',1)[1] for field in fields if field.startswith('playlist_cur_index:')))
        published=[entry['metadata']['title'] for entry in items()]
        return published==titles[:500]
    wait(shuffled_window,10,'LMS shuffled play order')
    print('PASS real LMS shuffle: upcoming window follows authoritative LMS shuffled status order',flush=True)
    cli('playlist shuffle 0',mac)

    source=context['Remote']((base/'library/0.flac').read_bytes())
    context['extra_remotes'].append(source)
    cli('stop',mac);wait(lambda:not items(),10,'remote natural release')
    cli('playlist play '+source.url,mac)
    wait(lambda:bool(items()),10,'finite remote natural publication')
    select_item(0)
    natural=fetch(items()[0],upper=True)
    load(fillers)
    cli('playlist add '+source.url,mac)
    wait(lambda:len(items())==3,10,'finite remote lazy window')
    remote_item=items()[2]
    assert 'size' not in remote_item['resource']
    select_item(2)
    before=len(proxy.records)
    deferred=fetch(remote_item,upper=True)
    assert deferred[:2]==natural[:2]
    assert len(deferred[1])<int(dict(deferred[0])['Content-Length'])
    selections=[r['request'] for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    assert selections==[mac+' playlist index 2'],selections
    assert 'kind=upper-bound' in (base/'bridge.log').read_text()
    print('PASS real LMS finite remote lazy window: upper-bound promise unchanged; natural and lazy headers/audio identical; short EOF without padding; one selection',flush=True)

    # Stall a routine read-only snapshot so the HTTP reader leaves before the
    # room worker can claim its queued activation.
    load(fillers+context['window_ids'][2:5])
    wait(lambda:len(items())>=4,10,'abandonment lazy window')
    entered=threading.Event()
    def hold_status(wire):
        if b' status ' in wire and not entered.is_set():
            entered.set();time.sleep(.7)
        return None
    proxy.request_hook=hold_status
    wait(entered.is_set,5,'worker in read-only snapshot')
    before=len(proxy.records)
    url=urlsplit(items()[2]['uri'])
    conn=socket.create_connection((url.hostname,url.port),timeout=5)
    conn.sendall(('GET '+url.path+('?' + url.query if url.query else '')+' HTTP/1.1\r\nHost: localhost\r\n\r\n').encode())
    time.sleep(.1);conn.close();time.sleep(.9)
    proxy.request_hook=None
    assert not [r for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    assert 'queue lazy abandoned' in (base/'bridge.log').read_text()
    print('PASS real LMS abandoned lazy GET: reader leaves before worker claim; no headers; zero playlist index commands; reader released',flush=True)

    load(fillers+context['window_ids'][2:5])
    wait(lambda:len(items())>=4,10,'deadline lazy window')
    before=len(proxy.records)
    # Fake slow LMS: acknowledge selection but withhold the corresponding
    # producer. All other traffic still reaches the private real LMS.
    def slow_start(wire):
        if b' playlist index 2' in wire and not wire.rstrip().endswith(b'?'):
            return wire
        return None
    proxy.request_hook=slow_start
    url=urlsplit(items()[2]['uri'])
    conn=socket.create_connection((url.hostname,url.port),timeout=5)
    start=time.monotonic()
    conn.sendall(('GET '+url.path+('?' + url.query if url.query else '')+' HTTP/1.1\r\nHost: localhost\r\n\r\n').encode())
    assert conn.recv(4096)==b''
    elapsed=time.monotonic()-start;conn.close()
    assert 2.8<elapsed<3.8,elapsed
    time.sleep(.2);proxy.request_hook=None
    selections=[r['request'] for r in proxy.records[before:] if ' playlist index ' in r['request'] and not r['request'].endswith(' ?')]
    assert selections==[mac+' playlist index 2'],selections
    assert 'queue lazy deadline' in (base/'bridge.log').read_text()
    print(f'PASS fake slow LMS lazy deadline: clean close without headers after {elapsed:.3f} s; one selection, no second command; logged and released',flush=True)
    cli('stop',mac)
    wait(lambda:not items(),10,'final release')
