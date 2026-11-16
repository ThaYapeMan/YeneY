# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Captured state and event contract through the production local API client."""
import os
import queue
import subprocess
import threading
import time
from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi


def main():
    owner = Coordinator()
    owner.realtime = True
    owner.transport_state = 'PLAYING'
    owner.position = dict(Track='2', TrackURI='owned', RelTime='00:00:10')
    api = LocalApi(owner, port=0)
    api.websocket_url = f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
    api.spike_every = 0
    process = subprocess.Popen([os.environ.get('YENEY_CAPTURE_STATE_FIXTURE', './speaker-websocket-fixture')], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               env=dict(os.environ, YENEY_WS_TEST_PORT=str(api.server.server_port)))
    journal = []
    threading.Thread(target=lambda: [journal.append(line) for line in process.stderr], daemon=True).start()
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line.rstrip('\n').split('\t')) for line in process.stdout], daemon=True).start()

    def wait(predicate, timeout=4):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            try: fields = lines.get(timeout=.1)
            except queue.Empty: continue
            if predicate(fields): return fields
        raise AssertionError('captured source deadline')

    try:
        value = wait(lambda f: f[4] == 'PLAYING')
        assert value[2] == '2' and value[6] == 'PLAYBACK_STATE_PLAYING', value
        time.sleep(2)
        while not lines.empty(): lines.get_nowait()
        errors = []
        for _ in range(30):
            value = wait(lambda f: f[4] == 'PLAYING')
            errors.append(float(value[1]) - (10000+max(0,float(value[0])-max(owner.position.stamp,api.buffering_until))*1000))
        assert max(abs(e) for e in errors) < 5, errors
        print('PASS G1 real state names recognise playing; steady source error below 5 ms', flush=True)
        owner.transport_state = 'PAUSED_PLAYBACK'
        paused = wait(lambda f: f[4] == 'PAUSED')
        held = float(paused[1])
        time.sleep(.2)
        paused = wait(lambda f: f[4] == 'PAUSED' and float(f[0]) > float(paused[0])+.15)
        assert abs(float(paused[1])-held) < 2
        revision = paused[5]
        for _ in range(3):
            api.emit_status(dict(playbackState='PLAYBACK_STATE_PAUSED', itemId='2', positionMillis=held))
        unchanged = wait(lambda f: float(f[0]) > float(paused[0])+.1)
        assert unchanged[5] == revision, (paused, unchanged)
        api.rtt_ms = 100
        api.emit_status(dict(playbackState='PLAYBACK_STATE_PAUSED', itemId='2', positionMillis=held-40,
                             previousItemId='2', previousPositionMillis=held))
        tiny = wait(lambda f: f[4] == 'PAUSED' and abs(float(f[1])-(held-40)) < 1)
        wait(lambda f: f[4] == 'PAUSED' and abs(float(f[1])-held) < 1 and float(f[0]) > float(tiny[0]))
        print('PASS G3 a 40 ms event seek resets immediately, before its contradicting poll', flush=True)
        api.emit_status(dict(playbackState='PLAYBACK_STATE_PLAYING', itemId='99', positionMillis=77777))
        wait(lambda f: f[2] == '99')
        wait(lambda f: f[2] == '2' and f[4] == 'PAUSED')
        api.rtt_ms = 10
        print('PASS G3 repeated event changes nothing; contradicting command answer wins', flush=True)
        journal_mark = len(journal)
        owner.transport_state = 'PLAYING'
        buffered = wait(lambda f: f[4] == 'TRANSITIONING')
        held = float(buffered[1])
        buffered2 = wait(lambda f: f[4] == 'TRANSITIONING' and float(f[0]) > float(buffered[0])+.1)
        assert abs(float(buffered2[1])-held) < 2, (buffered, buffered2)
        wait(lambda f: f[4] == 'PLAYING')
        assert not any('speaker seek ' in line for line in journal[journal_mark:]), journal[journal_mark:]
        print('PASS G4 buffering position held; transition is distinct from pause and fault', flush=True)
        api.state_override = 'PLAYBACK_STATE_IDLE'
        wait(lambda f: f[4] == 'STOPPED')
        api.state_override = 'FUTURE_STATE'
        wait(lambda f: f[4] == 'UNKNOWN' and f[6] == 'FUTURE_STATE')
        run = subprocess.run(['./yeney', '--position-source-check', '--ip', f'127.0.0.1:{api.server.server_port}', '--seconds', '1'], capture_output=True, text=True, timeout=10)
        assert run.returncode and 'raw=FUTURE_STATE mapped=UNKNOWN' in run.stdout, run.stdout
        assert sum('speaker unknown playbackState=FUTURE_STATE' in line for line in journal) == 1, journal
        print('PASS G2 playing, paused, buffering, idle and unknown; unknown diagnostic exits nonzero', flush=True)
        api.state_override = None
        api.emit_error(dict(errorCode='ERROR_PLAYBACK_FAILED', reason='fixture occurrence', itemId='2', httpStatus=403))
        wait(lambda f: f[4] == 'PLAYING')
        for code in ('ERROR_COMMAND_FAILED', 'ERROR_NOT_CAPABLE', 'ERROR_INVALID_OBJECT_ID'):
            api.command_error = code
            run = subprocess.run(['./yeney', '--position-source-check', '--ip', f'127.0.0.1:{api.server.server_port}', '--seconds', '1'], capture_output=True, text=True, timeout=10)
            assert run.returncode and 'command error code='+code in run.stdout, run.stdout
        api.command_error = None
        print('PASS G7 playbackError is informational; globalError command failures retain all three codes', flush=True)
        api.item_override = 'opaque-row'
        wait(lambda f: f[2] == 'opaque-row')
        api.item_override = None
        owner.position = dict(Track='3', TrackURI='owned-next', RelTime='00:00:00')
        wait(lambda f: f[2] == '3' and 'bitDepth=24 sampleRate=44100 lossless=true' in f[7])
        print('PASS G5 numeric item change and nonnumeric item accepted', flush=True)
        def change_during_check():
            time.sleep(.2)
            owner.position = dict(Track='4', TrackURI='owned-final', RelTime='00:00:00')
        change = threading.Thread(target=change_during_check)
        change.start()
        run = subprocess.run(['./yeney', '--position-source-check', '--ip', f'127.0.0.1:{api.server.server_port}', '--seconds', '1'], capture_output=True, text=True, timeout=10)
        change.join()
        for kind in ('playbackStatus', 'metadataStatus'):
            counts = [int(line.split('count=')[1]) for line in run.stdout.splitlines() if line.startswith('events type='+kind+' ')]
            assert counts and counts[0] > 0, run.stdout
        assert run.returncode == 0 and 'playing recognised=true' in run.stdout and 'itemId=3' in run.stdout, run.stdout
        assert 'residual_ms rms=' in run.stdout, run.stdout
        print('PASS G7 diagnostic mapped states, item, playing and residual; metadata quality retained', flush=True)
    finally:
        process.terminate(); process.wait(5); process.stdout.close(); process.stderr.close()
        api.close(); owner.close()


if __name__ == '__main__': main()
