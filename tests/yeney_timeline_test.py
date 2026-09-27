"""Replay device traces through unchanged stream code and production resume hooks.
Only steady-clock reads in temporary copies are replaced; repository code is untouched.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'sonos-lms.cpp').read_text()
def function(signature):
    start = source.index(signature); opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]
with tempfile.TemporaryDirectory(prefix='yeney-timeline-') as temp:
    temp = Path(temp)
    clock = '''#pragma once
#include <atomic>
#include <chrono>
struct TimelineClock {
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::steady_clock::duration;
    static inline std::atomic<long long> ms{0};
    static inline std::atomic<long long> us{0};
    static time_point now() { return time_point{} + std::chrono::milliseconds(ms.load()) + std::chrono::microseconds(us.load()); }
};
'''
    (temp / 'clock.h').write_text(clock)
    for name in ('sbstreamer.cpp', 'resume_state.h'):
        (temp / name).write_text('#include "clock.h"\n' + (ROOT / name).read_text().replace('std::chrono::steady_clock', 'TimelineClock'))
    (temp / 'production_resume.inc').write_text('\n'.join(function(s) for s in (
        'static void checkGetPairConfirmation(', 'static void ObserveDeviceTransport(', 'void ResumeSqueezeBox(', 'void ResumeSqueezeBoxGetPair(')))
    exe = temp / 'timeline'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(temp), '-I', str(ROOT),
                    '-Inoson/noson/src', '-Inoson/noson/public/noson',
                    str(ROOT / 'tests/yeney_timeline_fixture.cpp'), str(temp / 'sbstreamer.cpp'),
                    'sbencoder.cpp', 'sonos-position.cpp', 'upnp/encoded_buffer.cpp',
                    'noson/noson/libnoson.a', '-lFLAC++', '-lFLAC', '-lcrypto', '-lssl', '-lz', '-lpthread',
                    '-o', str(exe)], cwd=ROOT, check=True)
    for event_ms, second in [(21, 0), (5030, 1), (4990, 1), (-1, 0)]:
        subprocess.run([str(exe), str(event_ms), str(second)], check=True, timeout=10)

    for scenario in ('pair12', 'pair68', 'pair30', 'single20', 'spontaneous', 'playing', 'pause', 'late', 'unconfirmed'):
        subprocess.run([str(exe), scenario], check=True, timeout=10)
