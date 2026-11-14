# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
from pathlib import Path
import queue
import signal
import subprocess
import threading
import time
from queue_sonos_fake import Coordinator


def main():
    fixture = os.environ.get('YENEY_WS_FIXTURE', '.test-cache/timing-03/ws-fixture')
    owner = Coordinator()
    owner.realtime = True
    owner.transport_state = 'PLAYING'
    owner.position = dict(Track='1', TrackURI='owned-one', RelTime='00:00:10')
    from sonos_websocket_fake import LocalApi
    api = LocalApi(owner)
    process = subprocess.Popen([fixture], stdout=subprocess.PIPE, text=True)
    lines = queue.Queue()
    thread = threading.Thread(target=lambda: [lines.put(line) for line in process.stdout], daemon=True)
    thread.start()

    def wait(predicate, timeout=3):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                line = lines.get(timeout=.1).rstrip().split('\t')
            except queue.Empty:
                continue
            if predicate(float(line[0]), float(line[1]), line[2]):
                return float(line[0]), float(line[1])
        raise AssertionError('source deadline')

    try:
        wait(lambda t, p, i: p >= 10000)
        assert api.info_reads == 1 and len(api.handshakes) == 1
        assert {h['namespace'] for _, h in api.commands if h['command'] == 'subscribe'} == {'playback:1', 'playbackMetadata:1'}
        time.sleep(.6)
        while not lines.empty(): lines.get_nowait()
        errors = []
        until = time.monotonic() + 2
        while time.monotonic() < until:
            t, value = wait(lambda t, p, i: p > 0)
            truth = 10000 + (t - owner.position.stamp) * 1000
            errors.append(value - truth)
        assert max(abs(e) for e in errors) < 5, (min(errors), max(errors))
        api.outlier_poll = api.polls + 1
        until = time.monotonic() + .6
        while time.monotonic() < until:
            t, value = wait(lambda t, p, i: p > 0)
            truth = 10000 + (t - owner.position.stamp) * 1000
            assert abs(value - truth) < 5, (value, truth)
        print('PASS T1 client discards a 250 ms RTT answer with a 400 ms false position; reported error stays below 5 ms', flush=True)
        print('PASS T1 handshake/key/subprotocol, local info, status parsing, midpoint with 50 ms spikes, both subscriptions', flush=True)
        start = time.monotonic()
        owner.position['RelTime'] = '00:00:05'
        t, _ = wait(lambda t, p, i: 5000 <= p < 5400)
        assert t - start < .3, t - start
        print(f'PASS T8 client discontinuity detected in {(t-start)*1000:.3f} ms', flush=True)
        owner.transport_state = 'PAUSED_PLAYBACK'
        wait(lambda t, p, i: 5000 <= p < 6000)
        time.sleep(.25)
        before = api.polls
        start = time.monotonic()
        owner.position = dict(Track='2', TrackURI='owned-two', RelTime='00:00:01')
        wait(lambda t, p, i: i == 'owned-two' and p == 1000)
        assert time.monotonic() - start < .3
        assert api.polls > before
        print('PASS T1 metadata event triggers immediate paused poll', flush=True)
        api.group = 'loopback-group-2'
        start = time.monotonic()
        process.send_signal(signal.SIGUSR1)
        wait(lambda t, p, i: api.info_reads == 2 and p >= 0 and any(h['groupId'] == api.group for _, h in api.commands))
        assert time.monotonic() - start < 1
        assert any(h['groupId'] == api.group for _, h in api.commands)
        print('PASS T6 group change re-reads local info and follows group without a one-second gap', flush=True)
        api.disabled = True
        api.disconnect()
        wait(lambda t, p, i: p < 0)
        api.disabled = False
        wait(lambda t, p, i: p >= 0, 33)
        assert api.info_reads == 3
        print('PASS T2 runtime socket failure invalidates source and retries after 30 seconds', flush=True)
    finally:
        process.terminate()
        process.wait(5)
        process.stdout.close()
        api.close()
        owner.close()


if __name__ == '__main__':
    main()
