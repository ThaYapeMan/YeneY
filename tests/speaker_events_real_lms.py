# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Event-to-LMS alignment under captured local API answer-time profiles."""
import json
import time


def run(c):
    cli, mac, wait, sonos = (c[k] for k in ('cli', 'mac', 'wait', 'sonos'))
    sonos.realtime = True
    api = sonos.local_api
    cli('playlistcontrol cmd:load track_id:' + ','.join(c['window_ids'][:2]), mac)
    wait(lambda: sonos.transport_state == 'PLAYING' and len(c['items']()) == 2, 60, 'two event timing rows')
    wait(lambda: 'queue position source=websocket' in (c['base']/'bridge.log').read_text(), 10, 'precise source')
    results = []
    for profile in ('cable', 'wireless'):
        api.answer_profile = profile
        for enabled in (True, False):
            api.events_enabled = enabled
            c['select_item'](0)
            time.sleep(2)
            for action in ('app-seek', 'rewind', 'pause', 'resume', 'next', 'back'):
                if action == 'app-seek':
                    with sonos.lock: sonos.position['RelTime'] = '00:00:40'
                elif action == 'rewind':
                    with sonos.lock: sonos.position['RelTime'] = '00:00:00'
                elif action == 'pause': sonos.transport_state = 'PAUSED_PLAYBACK'
                elif action == 'resume': sonos.transport_state = 'PLAYING'
                else: c['select_item'](1 if action == 'next' else 0)
                start = time.monotonic()
                aligned = None
                while time.monotonic()-start < 5:
                    sent = time.monotonic()
                    value = float(cli('time ?', mac)[-1])*1000
                    received = time.monotonic()
                    with sonos.lock:
                        truth = sonos.position.milliseconds()
                        if sonos.transport_state == 'PLAYING': truth -= (time.monotonic()-(sent+received)/2)*1000
                    if abs(value-truth) <= 30:
                        aligned = (received-start)*1000
                        break
                    time.sleep(.005)
                row = dict(profile=profile, events=enabled, action=action, alignment_ms=aligned)
                results.append(row)
                print('MEASURE G3 '+json.dumps(row), flush=True)
                assert aligned is not None, row
                time.sleep(.7)
    status_path = c['base']/('stream-pacing-'+b'Study'.hex()+'.json')
    def status(): return json.loads(status_path.read_text())['playback']
    log = lambda: (c['base']/'bridge.log').read_text()
    api.events_enabled = True
    before = len(log())
    for _ in range(2):
        api.emit_error(dict(errorCode='ERROR_PLAYBACK_FAILED', reason='fixture occurrence', itemId='1', httpStatus=403))
        time.sleep(.1)
    wait(lambda: 'ERROR_PLAYBACK_FAILED' in status()['speaker_playback_error'], 3, 'last playbackError status')
    assert log()[before:].count('speaker playbackError errorCode=ERROR_PLAYBACK_FAILED') == 2
    assert sonos.transport_state == 'PLAYING'
    api.state_override = 'FUTURE_STATE'
    wait(lambda: status()['speaker_playback_state'] == 'FUTURE_STATE', 3, 'unknown state status')
    time.sleep(.2)
    assert log().count('speaker unknown playbackState=FUTURE_STATE') == 1
    api.state_override = None
    wait(lambda: status()['speaker_playback_state'] == 'PLAYBACK_STATE_PLAYING', 3, 'mapped playing restored')
    api.item_override = '999'
    wait(lambda: 'speaker itemId disagrees' in log(), 3, 'URI disagreement journal')
    assert status()['current_owned_item']
    api.item_override = None
    time.sleep(.2)
    api.emit_status(dict(playbackState='PLAYBACK_STATE_IDLE', itemId='1', positionMillis=180000,
                         previousItemId='1', previousPositionMillis=180000))
    wait(lambda: 'speaker finished itemId=1' in log(), 3, 'finished exact promise log')
    assert 'speaker seek itemId=' in log()
    assert 'previousPositionMillis=' in log()
    assert 'bitDepth=24 sampleRate=44100 lossless=true' in status()['speaker_quality']
    print('PASS G2/G5/G7 room unknown logged once and exposed, URI wins, previous positions and exact finish, two informational playback errors and quality status', flush=True)
    text = log()
    assert 'speaker quality bitDepth=24 sampleRate=44100 lossless=true' in text
    assert text.count('speaker unknown playbackState=') == 1
    assert 'rearm-loop room=' not in text
    print('PASS G3 all event/profile/action LMS time queries align within 30 ms', flush=True)
