"""Real HTTP/Slimproto tests with dense native 24-bit openings and audible clocks."""
import os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
namespace = {'__file__': str(ROOT/'tests/player_engine_test.py')}
exec((ROOT/'tests/player_engine_test.py').read_text().split('\nwith tempfile.TemporaryDirectory')[0], namespace)
Run, HTTP = namespace['Run'], namespace['HTTP']
import tempfile
import time
import random

def packets(run, event, start=0):
    return [p for p in run.lms.packets[start:] if p.get('event') == event]

with tempfile.TemporaryDirectory(prefix='yeney-start-lead-') as tmp:
    dense = random.Random(20261002).randbytes(44100*6*8)
    for mode in ('core','squeezelite'):
        os.environ['YENEY_START_LEAD_MS'] = '2000'
        os.environ['YENEY_DEBUG_STREAM'] = '1'
        run = Run(mode,Path(tmp))
        try:
            begin=time.monotonic()
            run.start(dense[:44100*6*3],fmt='p',bits=24)
            run.until(lambda: run.captures and len(run.captures[-1][1])>480000,timeout=1)
            assert time.monotonic()-begin<1.5
            assert run.lms.timer()['elapsed']==0
            run.decoded_track()
            marker=len(run.lms.packets)
            source=HTTP(dense); run.sources.append(source)
            run.lms.strm('s',source,fmt='p',bits=24)
            # Encoder reaches the queued boundary ahead of Sonos. Reporting
            # must retain the original track while the speaker still reports 0.
            time.sleep(1.6); assert run.lms.timer()['elapsed']==0
            assert not packets(run,'STMs',marker), packets(run,'STMs',marker)
            assert not packets(run,'STMu',marker) and not packets(run,'STMo',marker)
            time.sleep(max(0,3.3-(time.monotonic()-begin)))
            run.device.position=4
            run.lms.wait('STMs',6)
            assert run.device.counts.get('Play',0)==1, run.device.counts
            assert run.lms.timer()['elapsed']<1500
            # pause=stop produces a fresh response/burst, same stream ID.
            paused_elapsed=run.lms.timer()['elapsed']
            run.lms.strm('p'); run.lms.wait('STMp')
            run.until(lambda:run.device.counts.get('Stop',0)==1)
            before=len(run.captures)
            run.lms.strm('u'); run.lms.wait('STMr')
            run.until(lambda:len(run.captures)>before and len(run.captures[-1][1])>400000,timeout=1.5)
            assert run.lms.timer()['elapsed']==paused_elapsed, (mode,paused_elapsed,run.lms.timer()['elapsed'])
            # Explicit q/s restarts the track and creates another burst.
            run.lms.strm('q'); run.lms.wait('STMf')
            before=len(run.captures)
            run.start(dense,fmt='p',bits=24)
            run.until(lambda:len(run.captures)>before and len(run.captures[-1][1])>400000,timeout=1.5)
            assert run.lms.timer()['elapsed']==0
        finally:
            run.close()
        log=(run.base/'bridge.log').read_text()
        debug=[line for line in log.splitlines() if ': debug t=' in line]
        assert debug and any('final' in line for line in debug), log
        assert all(all(field in line for field in ('lead_ms=','bytes=','send_max_ms=','rtt_us=','rttvar_us=','snd_cwnd=','unacked=','retransmits=','total_retrans=','end=')) for line in debug),debug
        print(f'PASS: {mode}: dense 2 s HTTP burst; STMt/elapsed and gapless STMs remain audible; no early underrun; pause/u and q/s burst again; real TCP_INFO diagnostics',flush=True)
        for line in debug: print(line,flush=True)
    # Actual default-off path and invalid setting fallback, on a separate process.
    os.environ['YENEY_START_LEAD_MS']='invalid'
    os.environ.pop('YENEY_DEBUG_STREAM',None)
    # directory name must be unique but engine selection must remain real.
    with tempfile.TemporaryDirectory(prefix='yeney-lead-default-') as second:
        run=Run('core',Path(second))
        try:
            run.start(dense,fmt='p',bits=24)
            time.sleep(1.2)
            run.lms.timer()
        finally: run.close()
        log=(run.base/'bridge.log').read_text()
        assert ': debug t=' not in log and 'YENEY_START_LEAD_MS invalid; using 1000' in log,log
        print('PASS: stream diagnostics off by default; invalid lead falls back to 1000 ms',flush=True)
