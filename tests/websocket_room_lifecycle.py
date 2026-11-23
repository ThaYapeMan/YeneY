# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Five-minute real-room lifecycle contract; unchanged millisecond criteria."""
from contextlib import closing
import http.client
import json
import re
import statistics
import time
from urllib.parse import urlsplit
from xml.sax.saxutils import escape

from served_queue_audio import RowReader


def run(c):
    cli,mac,wait,sonos=(c[k] for k in ('cli','mac','wait','sonos'))
    sonos.realtime=True
    api=sonos.local_api
    api.ping_interval=60
    api.subscription_lifetime=90
    cli('playlistcontrol cmd:load track_id:'+','.join(c['window_ids'][:2]),mac)
    wait(lambda:sonos.transport_state=='PLAYING' and len(c['items']())==2,60,'lifecycle long rows')
    path=c['base']/('stream-pacing-'+b'Study'.hex()+'.json')
    def status():return json.loads(path.read_text())['playback']
    wait(lambda:status()['position_source']=='websocket',10,'lifecycle precise source')
    time.sleep(2)
    logpath=c['base']/'bridge.log'
    mark=len(logpath.read_text())
    data=[];actions=set();start=due=time.monotonic()
    try:
        while time.monotonic()-start<300:
            elapsed=time.monotonic()-start
            for second,label,action in ((150,'forced-reconnect',api.disconnect),
                                        (210,'group-change',lambda:api.regroup('loopback-lifecycle-group-2'))):
                if elapsed>=second and label not in actions:
                    actions.add(label);action()
                    print(f'LIFECYCLE action={label} elapsed_s={elapsed:.6f}',flush=True)
            sent=time.monotonic();value=float(cli('time ?',mac)[-1])*1000;received=time.monotonic()
            midpoint=(sent+received)/2
            with sonos.lock:
                truth=sonos.position.milliseconds()-(time.monotonic()-midpoint)*1000
            current=status()
            assert current['position_source']=='websocket' and current['position_precision']=='ms',current
            data.append((midpoint-start,value-truth))
            due+=.25;time.sleep(max(0,due-time.monotonic()))
        for index in range(5):
            rows=[(t,v) for t,v in data if index*60<=t<(index+1)*60]
            values=[v for _,v in rows];mt=statistics.mean(t for t,_ in rows);mv=statistics.mean(values)
            creep=sum((t-mt)*(v-mv) for t,v in rows)/sum((t-mt)**2 for t,_ in rows)
            result=dict(window=index+1,n=len(rows),median_ms=statistics.median(values),max_ms=max(map(abs,values)),
                        within_30_pct=100*sum(abs(v)<=30 for v in values)/len(values),
                        jumps_over_100_ms=sum(abs(b[1]-a[1])>100 for a,b in zip(rows,rows[1:])),creep_ms_per_s=creep)
            print('MEASURE LIFECYCLE '+json.dumps(result),flush=True)
            assert abs(result['median_ms'])<=10 and result['within_30_pct']>=95,result
            assert result['jumps_over_100_ms']==0 and abs(creep)<.1,result
        journal=logpath.read_text()[mark:]
        assert 'queue position source=upnp' not in journal,journal[-10000:]
        pings=sum(r['opcode']==9 and r['direction']=='sent' for r in api.controls)
        assert pings>=2,api.controls
        assert 'speaker reconnecting' in journal and 'group moved' in journal,journal[-10000:]
        print(f'ROOM-LIFECYCLE connections={len(api.handshakes)}', flush=True)
        assert len(api.handshakes) == 3, api.handshakes  # initial, forced reconnect, new group
        print(f'PASS five-minute room: source=websocket throughout; pings={pings}; forced reconnect, group resolution and shortened subscription lifetime; unchanged timing windows',flush=True)
    finally:
        (c['base']/'websocket-lifecycle.tsv').write_text('elapsed_s\tlms_minus_true_ms\n'+''.join(f'{t:.9f}\t{v:.9f}\n' for t,v in data))


def run_transition(c, case):
    cli, mac, wait, sonos, items, base = (c[key] for key in ('cli', 'mac', 'wait', 'sonos', 'items', 'base'))
    log = lambda: (base / 'bridge.log').read_text()
    status = lambda: json.loads((base / ('stream-pacing-' + b'Study'.hex() + '.json')).read_text())['playback']
    if case == 'final-end':
        cli('playlistcontrol cmd:load track_id:' + c['window_ids'][0], mac)
        wait(lambda: len(items()) == 1 and sonos.transport_state == 'PLAYING', 15, 'last row playing')
        c['select_item'](0)
        uri = urlsplit(items()[0]['uri'])
        with closing(http.client.HTTPConnection(uri.netloc, timeout=8)) as conn:
            conn.request('GET', uri.path)
            wav = RowReader(conn.getresponse()).read_wav()
        assert len(wav) == int.from_bytes(wav[4:8], 'little') + 8
        rate = int.from_bytes(wav[24:28], 'little')
        end_seconds = (len(wav) - 44) // (rate * 6)
        with sonos.lock:
            sonos.position['RelTime'] = f'00:00:{end_seconds - 1:02}'
        wait(lambda: float(cli('time ?', mac)[-1]) >= end_seconds - 2, 8, 'final renderer position observed')
        marks = len(log()), len(c['proxy'].records), len(sonos.requests)
        with sonos.lock:
            sonos.transport_state = 'STOPPED'
            sonos.position_variant = 'not-implemented'
        time.sleep(30)
        text = log()[marks[0]:]
        decisions = re.findall(r'queue end decision=natural occurrence=\d+', text)
        lms_commands = [row['request'] for row in c['proxy'].records[marks[1]:]
                        if re.search(r' (time|playlist index|pause|stop)( |$)', row['request'])
                        and not row['request'].endswith(' ?')]
        speaker_commands = [row[0] for row in sonos.requests[marks[2]:]
                            if row[0] in ('Seek', 'Play', 'Stop', 'Pause', 'AddURIToQueue',
                                          'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
        print(f'FINAL-END decisions={len(decisions)} journal_lines={len(decisions)} '
              f'lms_commands={lms_commands} speaker_commands={speaker_commands} '
              f'room_state={status()["state"]}', flush=True)
        assert len(decisions) == 1 and not lms_commands and not speaker_commands
        assert status()['state'] == 'STOPPED'
        return
    if case == 'status':
        wait(lambda: bool(getattr(sonos, 'callback', '')), 8, 'transport subscription')
        sonos.notify_transport({'TransportStatus': 'ERROR_LOST_CONNECTION'})
        wait(lambda: 'ERROR_LOST_CONNECTION' in log(), 5, 'transport error delivered by event')
        cli('playlistcontrol cmd:load track_id:' + c['window_ids'][0], mac)
        wait(lambda: status().get('transport_status') == 'OK', 10, 'poll status published in room file')
        wait(lambda: re.search(r'\| OK +\| PLAYING +\|', log()), 10, 'poll recovery in status table')
        print('PASS transport recovery: current OK visible in status table and room file', flush=True)
        return

    assert case == 'persistent', case
    sonos.realtime = True
    api = sonos.local_api
    ids = ','.join(c['window_ids'][:2])

    def topology(sequence):
        wait(lambda: bool(getattr(sonos, 'topology_callback', '')), 8, 'topology subscription')
        url = urlsplit(sonos.topology_callback)
        topology_xml = ('<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="RINCON_TEST" ID="test">'
                        '<ZoneGroupMember UUID="RINCON_TEST" ZoneName="Study" '
                        f'Location="http://127.0.0.1:{sonos.server_port}/description.xml"/>'
                        '</ZoneGroup></ZoneGroups></ZoneGroupState>')
        body = ('<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><ZoneGroupState>' +
                escape(topology_xml) + '</ZoneGroupState></e:property></e:propertyset>')
        with closing(http.client.HTTPConnection(url.netloc, timeout=3)) as conn:
            conn.request('NOTIFY', url.path, body, headers={
                'NT': 'upnp:event', 'NTS': 'upnp:propchange', 'SID': 'uuid:binary', 'SEQ': str(sequence)})
            assert conn.getresponse().status == 200

    cli('playlistcontrol cmd:load track_id:' + ids, mac)
    wait(lambda: len(items()) == 2 and status()['position_source'] == 'websocket', 10, 'two playing rows')
    mark = len(log())
    topology(1)
    cli('stop', mac)
    wait(lambda: not items() and sonos.transport_state == 'STOPPED', 8, 'room stopped')
    topology(2)
    wait(lambda: bool(api.control_senders), 8, 'idle socket ready')
    ping_mark = len(log())
    started = time.monotonic()
    api.control(9, b'idle-room-check')
    while time.monotonic() - started < 1 and 'websocket control=pong direction=sent' not in log()[ping_mark:]:
        time.sleep(.02)
    idle_journal_delay = (time.monotonic() - started) * 1000
    idle_journal_seen = 'websocket control=pong direction=sent' in log()[ping_mark:]
    cli('playlistcontrol cmd:load track_id:' + ids, mac)
    wait(lambda: len(items()) == 2 and sonos.transport_state == 'PLAYING', 8, 'room played again')
    wait(lambda: status()['state'] == 'PLAYING' and float(cli('time ?', mac)[-1]) >= 1,
         8, 'restarted stream is audible before pause')
    pause_mark = len(log())
    cli('pause 1', mac)
    wait(lambda: status()['state'] == 'PAUSED' and 'core: STMp' in log()[pause_mark:],
         8, 'pause acknowledged by LMS')
    cli('pause 0', mac)
    wait(lambda: sonos.transport_state == 'PLAYING', 8, 'room resumed')
    cli('playlist index 1', mac)
    wait(lambda: status()['title'] == 'Title & 1', 8, 'track changed')
    topology(3)
    transition = len(log())
    sonos.position_variant = 'not-implemented'
    api.position_variant = 'missing-position'
    cli('time 12', mac)
    time.sleep(4)
    api.position_variant = ''
    sonos.position_variant = ''
    wait(lambda: status()['position_source'] == 'websocket' and status()['title'] == 'Title & 1',
         8, 'post-transition track and clock')
    api.rtt_ms = 260
    time.sleep(3.5)
    api.rtt_ms = 0
    text = log()[transition:]
    print(f'ROOM-SEQUENCE connections={len(api.handshakes)} idle_journal_seen={idle_journal_seen} '
          f'idle_journal_delay_ms={idle_journal_delay:.3f} upnp_fallbacks={text.count("position source=upnp")}',
          flush=True)
    assert len(api.handshakes) == 1 and 'speaker reconnecting' not in log()[mark:], log()[mark:]
    assert idle_journal_seen and idle_journal_delay < 1000, log()[ping_mark:]
    assert 'speaker position unavailable' in text and 'position source=upnp' not in text, text
    assert 'relay fault=' not in text and 'end decision=fault' not in text
