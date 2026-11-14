# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""TIMING-03: real LMS CLI against the fake's independent continuous clock."""
import json
import os
from pathlib import Path
import statistics
import time


def run(c):
    cli, mac, wait, sonos, items = (c[k] for k in ('cli', 'mac', 'wait', 'sonos', 'items'))
    sonos.realtime = True
    cli('playlistcontrol cmd:load track_id:' + ','.join(c['window_ids'][:2]), mac)
    wait(lambda: sonos.transport_state == 'PLAYING' and len(items()) == 2, 60, 'two long timing rows')
    log = lambda: (c['base'] / 'bridge.log').read_text()
    source = os.environ.get('YENEY_POSITION_SOURCE', 'auto')
    precise = source != 'upnp'
    if precise:
        wait(lambda: 'queue position source=websocket' in log(), 10, 'websocket selected')
    time.sleep(2)
    rows = []
    mark = len(sonos.requests)

    def sample():
        sent = time.monotonic()
        value = float(cli('time ?', mac)[-1]) * 1000
        received = time.monotonic()
        midpoint = (sent + received) / 2
        with sonos.lock:
            truth = sonos.position.milliseconds() - (time.monotonic() - midpoint) * 1000
        return midpoint, value - truth

    def measure(label, duration):
        data = []
        start = due = time.monotonic()
        while time.monotonic() - start < duration:
            data.append(sample())
            due += .25
            time.sleep(max(0, due - time.monotonic()))
        values = [v for _, v in data]
        mean_t = statistics.mean(t for t, _ in data)
        mean_v = statistics.mean(values)
        creep = sum((t-mean_t)*(v-mean_v) for t, v in data) / sum((t-mean_t)**2 for t, _ in data)
        result = dict(source=source, phase=label, n=len(data), median_ms=statistics.median(values),
                      within_30_pct=100*sum(abs(v)<=30 for v in values)/len(values),
                      jumps_over_100_ms=sum(abs(b[1]-a[1])>100 for a,b in zip(data,data[1:])), creep_ms_per_s=creep)
        print('MEASURE TIMING03 ' + json.dumps(result), flush=True)
        rows.extend((label, t, v) for t, v in data)
        if precise:
            assert abs(result['median_ms']) <= 10, result
            assert result['within_30_pct'] >= 95, result
            assert result['jumps_over_100_ms'] == 0, result
            assert abs(creep) < .1, result
        return result

    try:
        measure('steady', 120)
        for label, second in [('app-seek', 40), ('rewind', 0)]:
            if label == 'rewind':
                cli('time 20', mac)
                wait(lambda: any(r[0]=='Seek' and r[1].get('Unit')=='REL_TIME' for r in sonos.requests[mark:]), 10, 'LMS seek')
                time.sleep(1)
            logmark = len(log())
            climark = len(c['proxy'].records)
            soapmark = len(sonos.requests)
            start = time.monotonic()
            with sonos.lock:
                sonos.position['RelTime'] = f'00:00:{second:02}'
            wait(lambda: 'speaker-seek ' in log()[logmark:], 3, label + ' detection')
            delay = time.monotonic() - start
            print(f'MEASURE TIMING03 source={source} {label}_delay_ms={delay*1000:.3f}', flush=True)
            if precise:
                assert delay < .3, delay
            if label == 'rewind':
                wait(lambda: any(' time 0' in r['request'] for r in c['proxy'].records[climark:]), 3, 'one rewind time command')
                assert len([r for r in c['proxy'].records[climark:] if ' time ' in r['request'] and not r['request'].endswith(' ?')]) == 1
                assert not [r for r in sonos.requests[soapmark:] if r[0] == 'Seek']
            time.sleep(2)
            measure('after-' + label, 8)
        c['select_item'](1)
        time.sleep(2)
        measure('boundary', 8)
        polls = sum(r[0]=='GetPositionInfo' for r in sonos.requests[mark:])
        print(f'MEASURE TIMING03 source={source} UPnP_polls={polls}', flush=True)
    finally:
        output = c['base'] / 'timing03.tsv'
        output.write_text('phase\tmidpoint_mono\tlms_minus_true_ms\n' + ''.join(f'{label}\t{t:.9f}\t{v:.6f}\n' for label,t,v in rows))
        print('TIMING03 raw_tsv=' + str(output), flush=True)
