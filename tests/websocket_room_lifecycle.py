# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Five-minute real-room lifecycle contract; unchanged millisecond criteria."""
import json
import statistics
import time


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
        print(f'PASS five-minute room: source=websocket throughout; pings={pings}; forced reconnect, group resolution and shortened subscription lifetime; unchanged timing windows',flush=True)
    finally:
        (c['base']/'websocket-lifecycle.tsv').write_text('elapsed_s\tlms_minus_true_ms\n'+''.join(f'{t:.9f}\t{v:.9f}\n' for t,v in data))
