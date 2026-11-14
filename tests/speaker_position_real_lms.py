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
    transitions = []
    detection_failures = []
    mark = len(sonos.requests)
    measurement_started = time.monotonic()

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
            if duration >= 120:
                assert abs(creep) < .1, result
        return result

    try:
        measure('steady', float(os.environ.get('YENEY_TIMING03_DURATION', '120')))
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
            aligned = None
            while time.monotonic() - start < 2:
                point = sample()
                if abs(point[1]) <= 30:
                    aligned = time.monotonic() - start
                    break
                time.sleep(.01)
            transitions.append((label, aligned))
            print(f'MEASURE TIMING03 source={source} {label}_LMS_alignment_ms={aligned*1000 if aligned is not None else None}', flush=True)
            if precise and delay >= .3:
                detection_failures.append((label, delay))
            if label == 'rewind':
                wait(lambda: any(' time 0' in r['request'] for r in c['proxy'].records[climark:]), 3, 'one rewind time command')
                assert len([r for r in c['proxy'].records[climark:] if ' time ' in r['request'] and not r['request'].endswith(' ?')]) == 1
                assert not [r for r in sonos.requests[soapmark:] if r[0] == 'Seek']
            time.sleep(2)
            measure('after-' + label, 8)
        with sonos.lock:
            sonos.position['RelTime'] = '00:02:58'
        time.sleep(2)
        start = time.monotonic()
        c['select_item'](1)
        aligned = None
        while time.monotonic() - start < 2:
            point = sample()
            if abs(point[1]) <= 30:
                aligned = time.monotonic() - start
                break
            time.sleep(.01)
        transitions.append(('boundary', aligned))
        print(f'MEASURE TIMING03 source={source} boundary_LMS_alignment_ms={aligned*1000 if aligned is not None else None}', flush=True)
        time.sleep(2)
        measure('boundary', 8)
        polls = sum(r[0]=='GetPositionInfo' for r in sonos.requests[mark:])
        print(f'MEASURE TIMING03 source={source} UPnP_polls={polls} UPnP_polls_per_minute={polls*60/(time.monotonic()-measurement_started):.3f}', flush=True)
        if precise:
            assert not detection_failures, detection_failures
            assert all(delay is not None and delay < .3 for _, delay in transitions), transitions
    finally:
        output = c['base'] / 'timing03.tsv'
        output.write_text('phase\tmidpoint_mono\tlms_minus_true_ms\n' + ''.join(f'{label}\t{t:.9f}\t{v:.6f}\n' for label,t,v in rows))
        print('TIMING03 raw_tsv=' + str(output), flush=True)
