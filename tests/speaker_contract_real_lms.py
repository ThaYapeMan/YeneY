# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""The same observable room contracts against a baseline or current binary."""
import json
import os
import subprocess
import time


def run(c):
    cli, mac, sonos, wait = (c[k] for k in ('cli','mac','sonos','wait'))
    sonos.realtime = True
    api = sonos.local_api
    api.spike_every = 0
    cli('playlistcontrol cmd:load track_id:'+','.join(c['window_ids'][:2]), mac)
    wait(lambda: sonos.transport_state == 'PLAYING' and len(c['items']()) == 2, 60, 'contract rows')
    log = lambda: (c['base']/'bridge.log').read_text()
    wait(lambda: 'queue position source=websocket' in log(), 10, 'contract websocket')
    status_path = c['base']/('stream-pacing-'+b'Study'.hex()+'.json')
    def status(): return json.loads(status_path.read_text())['playback']
    binary = os.environ.get('YENEY_ACCEPTANCE_BINARY', './yeney')
    check = lambda: subprocess.run([binary,'--position-source-check','--ip','127.0.0.1','--seconds','1'], capture_output=True, text=True, timeout=10)
    results = []
    def record(label, okay):
        results.append(okay)
        print(('PASS ' if okay else 'FAIL ')+label, flush=True)
    time.sleep(1)
    diagnostic = check()
    record('G1 real state names: playing recognised and playing residual', diagnostic.returncode == 0 and 'playing recognised=true' in diagnostic.stdout and 'residual_ms rms=' in diagnostic.stdout)
    api.state_override = 'FUTURE_STATE'
    diagnostic = check()
    time.sleep(.2)
    record('G2 unknown: nonzero check, raw status and one journal line', diagnostic.returncode != 0 and status().get('speaker_playback_state') == 'FUTURE_STATE' and log().count('speaker unknown playbackState=FUTURE_STATE') == 1)
    api.state_override = None
    time.sleep(.5)
    api.rtt_ms = 500
    mark = api.polls
    wait(lambda: api.polls > mark, 3, 'slow command in flight')
    time.sleep(.02)
    with sonos.lock: sonos.position['RelTime'] = '00:00:10'
    start = time.monotonic()
    aligned = False
    while time.monotonic()-start < .2:
        value = float(cli('time ?', mac)[-1])*1000
        if abs(value-sonos.position.milliseconds()) <= 30: aligned = True; break
        time.sleep(.005)
    record('G3 event position reaches LMS before the slow confirming answer', aligned)
    api.rtt_ms = 10
    time.sleep(.7)
    api.state_override = 'PLAYBACK_STATE_BUFFERING'
    time.sleep(.15)
    record('G4 buffering is exposed as transition, with no pause or fault', status().get('speaker_playback_state') == 'PLAYBACK_STATE_BUFFERING' and sonos.transport_state == 'PLAYING' and not status().get('last_fault'))
    api.state_override = None
    api.item_override = '999'
    time.sleep(.3)
    record('G5 numeric row disagreement logged once; exact owned URI retained', log().count('speaker itemId disagrees') == 1 and bool(status().get('current_owned_item')))
    api.item_override = None
    c['select_item'](1)
    time.sleep(.6)
    for _ in range(2):
        api.emit_error(dict(errorCode='ERROR_PLAYBACK_FAILED', reason='contract error', itemId='2', httpStatus=403))
        time.sleep(.1)
    record('G7 metadata quality and each playbackError retained without a source failure',
        'bitDepth=24 sampleRate=44100 lossless=true' in status().get('speaker_quality','') and
        'ERROR_PLAYBACK_FAILED' in status().get('speaker_playback_error','') and
        log().count('speaker playbackError errorCode=ERROR_PLAYBACK_FAILED') == 2 and
        sonos.transport_state == 'PLAYING')
    assert all(results), results
