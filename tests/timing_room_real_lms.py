# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Measure consumer timing after one renderer or LMS action."""
import http.client
import json
import os
import statistics
import time


def start_room(c):
    sonos = c['sonos']
    sonos.realtime = True
    c['cli']('playlistcontrol cmd:load track_id:' + ','.join(c['window_ids'][:2]), c['mac'])
    c['wait'](lambda: sonos.transport_state == 'PLAYING' and len(c['items']()) == 2
              and float(c['cli']('time ?', c['mac'])[-1]) > .5, 15, 'playing timing fixture')
    sonos.local_api.buffering_seconds = .38


def app_position(sonos, second):
    with sonos.lock:
        sonos.position['RelTime'] = f'00:00:{second:02}'

def transport(sonos, state):
    with sonos.lock:
        sonos.position.latch_milliseconds(sonos.position.milliseconds())
        sonos.transport_state = state


def query_clock(c):
    sonos = c['sonos']
    sent = time.monotonic()
    value = 1000 * float(c['cli']('time ?', c['mac'])[-1])
    received = time.monotonic()
    with sonos.lock:
        now = time.monotonic()
        held = sonos.transport_state != 'PLAYING' or now < sonos.local_api.buffering_until
        truth = sonos.position.milliseconds()
        if not held:
            truth -= 1000 * (now - (sent + received) / 2)
    return received, value, truth, held


def run_change(c, case):
    profile, action = case.split('/', 1)
    start_room(c)
    sonos, cli, mac = c['sonos'], c['cli'], c['mac']
    sonos.local_api.answer_profile = profile
    if action == 'resume':
        transport(sonos, 'PAUSED_PLAYBACK')
        c['wait'](lambda: cli('mode ?', mac)[-1] == 'pause', 3, 'LMS sees pause')
    time.sleep(2 if profile == 'study-replay' else .4)
    start = time.monotonic()
    if action == 'skip':
        c['select_item'](1)
    elif action == 'app-scrub':
        app_position(sonos, 12)
    elif action == 'lms-scrub':
        cli('time 12', mac)
    elif action == 'pause':
        transport(sonos, 'PAUSED_PLAYBACK')
    elif action == 'resume':
        transport(sonos, 'PLAYING')
    else:
        raise AssertionError(action)
    samples = []
    while time.monotonic() - start < 1.6:
        at, value, truth, held = query_clock(c)
        samples.append(((at-start)*1000, value, truth, held))
        time.sleep(.01)
    last_outside = max((i for i, (_, value, truth, _) in enumerate(samples) if abs(value-truth) > 30), default=-1)
    stable = samples[last_outside+1][0] if last_outside+1 < len(samples) else None
    held_samples = [(at, value, truth) for at, value, truth, held in samples if held]
    max_held_error = max((abs(value-truth) for _, value, truth in held_samples), default=None)
    held_advances = sum(
        left[3] and right[3] and abs(left[2]-right[2]) < 1 and abs(left[1]-left[2]) <= 30
        and abs(right[1]-right[2]) <= 30 and right[1]-left[1] > 3
        for left, right in zip(samples, samples[1:]))
    row = dict(profile=profile, action=action, stable_ms=stable, held_samples=len(held_samples),
               max_held_error_ms=max_held_error, held_advances=held_advances)
    print('MEASURE TIMING03E ' + json.dumps(row), flush=True)
    trace = c['base']/'timing03e.tsv'
    trace.write_text('after_action_ms\tlms_ms\ttruth_ms\theld\n' +
                     ''.join(f'{at:.6f}\t{value:.6f}\t{truth:.6f}\t{int(held)}\n'
                             for at, value, truth, held in samples))
    if not os.environ.get('YENEY_ACCEPTANCE_BINARY'):
        assert stable is not None and stable <= 500, row


def run_steady(c, profile):
    start_room(c)
    c['sonos'].local_api.answer_profile = profile
    time.sleep(3)
    errors = []
    start = time.monotonic()
    for i in range(480):
        time.sleep(max(0, start + i * .25 - time.monotonic()))
        at, value, truth, held = query_clock(c)
        assert not held
        errors.append((at-start, value-truth))
    coordinates = [value for _, value in errors]
    mean_t = statistics.mean(t for t, _ in errors)
    mean_error = statistics.mean(coordinates)
    creep = sum((t-mean_t)*(error-mean_error) for t, error in errors) / sum((t-mean_t)**2 for t, _ in errors)
    result = dict(profile=profile, samples=len(errors), within_30_pct=100*sum(abs(v)<=30 for v in coordinates)/len(errors),
                  median_ms=statistics.median(coordinates), creep_ms_per_s=creep,
                  jumps_over_100_ms=sum(abs(b-a)>100 for a, b in zip(coordinates, coordinates[1:])))
    print('MEASURE TIMING03E steady ' + json.dumps(result), flush=True)
    path = c['base']/('stream-pacing-' + b'Study'.hex() + '.json')
    status = json.loads(path.read_text())['playback']
    print('TIMING03E room position ' + json.dumps({key: value for key, value in status.items()
                                                 if key.startswith('position_')}), flush=True)
    if not os.environ.get('YENEY_ACCEPTANCE_BINARY'):
        assert result['within_30_pct'] >= 99 and abs(result['median_ms']) <= 3, result
        assert result['jumps_over_100_ms'] == 0 and abs(creep) < .1, result
        assert status['position_source'] == 'websocket', status
        assert status['position_link'] == ('even' if profile == 'cable' else 'uneven'), status
        assert status['position_precision_ms'] >= 0 and status['position_trusted_answer_age_ms'] >= 0, status


def comet(messages):
    connection = http.client.HTTPConnection('127.0.0.1', 9000, timeout=4)
    try:
        connection.request('POST', '/cometd', json.dumps(messages), {'Content-Type': 'application/json'})
        response = connection.getresponse()
        assert response.status == 200, response.status
        return json.loads(response.read())
    finally:
        connection.close()


def run_subscription(c, action):
    start_room(c)
    sonos = c['sonos']
    if action == 'backward':
        app_position(sonos, 12)
        c['wait'](lambda: float(c['cli']('time ?', c['mac'])[-1]) > 12,
                  3, 'initial scrub reached LMS')
    time.sleep(1.5)
    handshake = comet([dict(channel='/meta/handshake', version='1.0',
                            supportedConnectionTypes=['long-polling'], id='1')])[0]
    assert handshake['successful'], handshake
    client = handshake['clientId']
    channel = f'/{client}/slim/playerstatus/{c["mac"]}'
    listen = comet([dict(channel='/meta/subscribe', clientId=client, subscription=channel, id='2')])
    assert listen[0]['successful'], listen
    subscribe = comet([dict(channel='/slim/subscribe', clientId=client, id='2', data=dict(
        request=[c['mac'], ['status', '-', 1, 'subscribe:0']], response=channel))])
    assert any(reply.get('successful') for reply in subscribe), subscribe
    connect = dict(channel='/meta/connect', clientId=client, connectionType='long-polling',
                   advice=dict(timeout=100), id='3')
    initial = next((reply['data'] for reply in subscribe if reply['channel'] == channel), None)
    deadline = time.monotonic() + 1
    while initial is None and time.monotonic() < deadline:
        replies = comet([connect])
        initial = next((reply['data'] for reply in replies if reply['channel'] == channel), None)
    assert initial is not None, (listen, subscribe)
    start = time.monotonic()
    if action == 'skip':
        c['select_item'](1)
    else:
        app_position(sonos, 12 if action == 'forward' else 4)
    clock_ms = subscription_ms = None
    notifications = []
    while time.monotonic() - start < 3.5:
        at, value, truth, held = query_clock(c)
        if clock_ms is None and not held and abs(value-truth) <= 30:
            clock_ms = 1000 * (at-start)
        for response in comet([connect]):
            if response['channel'] != channel:
                continue
            data = response['data']
            age_ms = (time.monotonic()-start)*1000
            notifications.append(dict(delay_ms=age_ms, position_s=data.get('time'),
                                      index=data.get('playlist_cur_index')))
            if subscription_ms is None:
                if action == 'skip':
                    changed = data.get('playlist_cur_index') != initial.get('playlist_cur_index')
                else:
                    changed = abs(float(data.get('time', 0)) - query_clock(c)[2] / 1000) <= .3
                if changed:
                    subscription_ms = age_ms
    print('MEASURE TIMING03E subscription ' + json.dumps(dict(
        action=action, direct_query_ms=clock_ms, subscription_ms=subscription_ms,
        observed_ms=(time.monotonic()-start)*1000, notifications=notifications)), flush=True)
    assert clock_ms is not None
    if action == 'skip':
        assert subscription_ms is not None
