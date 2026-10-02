"""Replay S7#2 through ConnectionPosition and the production core STAT serializer."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
replay=r'''

#include "position_state.h"
#include "third_party/yeney-core/core/protocol.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
static ConnectionPosition position;
extern "C" {
uint64_t get_sonos_audible_frames(uint32_t rate) { return position.audibleFrames(rate); }
void report_init() {} void report_close() {}
uint32_t replay_device(uint32_t rate, uint32_t decoded) {
    auto heard=position.audibleFrames(rate); return decoded > heard ? decoded-heard : 0;
}
uint32_t report(uint32_t now,uint32_t rate,uint32_t decoded,uint32_t device,uint32_t) {
    yeney::Status status; status.jiffies=now; status.elapsed=uint64_t(decoded-device)*1000/rate;
    auto wire=yeney::statusPacket("STMt",status,0);
    assert(wire.size()==61);
    uint32_t ms=0; for(unsigned i=51;i<55;++i) ms=(ms<<8)|wire[i];
    uint32_t seconds=0; for(unsigned i=45;i<49;++i) seconds=(seconds<<8)|wire[i];
    assert(seconds==ms/1000); return ms;
}
}
int main() {
    report_init();
    for (unsigned rate : {44100u, 48000u}) for (bool lateReset : {false, true}) for (unsigned pollLag : {0u, 300u}) {
        position.reset(4);
        const unsigned epoch = rate + (lateReset ? 20000 : 0) + pollLag * 100;
        // 15:04:12.678 GET #7; 13.932 close (1.254 s); 13.966 GET #8.
        position.connection(4, 7, 0); position.pcm(4, 7, 0, 0);
        auto oldToken = position.token();
        unsigned last = 0, speaker = 0;
        // Journal: PLAYING 14.868; RelTime 1 at 16.384, then 2 at
        // 17.396, 3 at 18.408, 4 at 19.421, 5 at 20.432, 6 at 21.434.
        const unsigned polls[] = {3706, 4718, 5730, 6743, 7754, 8756};
        unsigned nextPoll = 0;
        for (unsigned now = 0; now <= 9000; ++now) {
            // PlayStream's metadata path may reset after the first PCM;
            // exercise the observed order as well as the opposite race.
            if (lateReset && now == 4) position.reset(4);
            if (now == 1288) {
                position.connection(4, 8, now);
                position.pcm(4, 8, rate * 2, now); // buffered, not yet audible
            }
            if (now == 1500) position.poll(oldToken, 17000, now);
            if (now == 2325) position.poll(position.token(), 0, now);
            if (nextPoll < 6 && now == polls[nextPoll]) {
                speaker = ++nextPoll * 1000;
            }
            // Offset bridge polling from independent speaker observations too.
            if (nextPoll && now == polls[nextPoll - 1] + pollLag)
                position.poll(position.token(), speaker, now);
            if (now == 6000) position.poll(position.token(), 1000, now); // stale lower value
            if (now % 50) continue;
            unsigned decoded = (now + 3000) * uint64_t(rate) / 1000;
            unsigned ms = report(epoch + now, rate, decoded, replay_device(rate, decoded), epoch);
            assert(ms >= last);
            assert(ms <= speaker + 500);
            if (now >= 2190) assert(ms + 1000 >= speaker);
            last = ms;
        }
        // A new track must reset the STAT clamp, even if the prior track ran.
        position.reset(5);
        unsigned decoded = rate;
        assert(report(epoch + 10000, rate, decoded, replay_device(rate, decoded), epoch + 10000) == 0);
        printf("PASS: S7#2 replay at %u Hz, late reset %d, poll lag %u ms: GET closes at 1.254 s, reconnect at 1.288 s; real STAT monotonic, ahead <= 0.5 s, PLAYING lag <= 1 s\n", rate, lateReset, pollLag);
    }
    report_close();
}
'''
with tempfile.TemporaryDirectory(prefix='sonos-position-replay-') as tmp:
    src=Path(tmp)/'replay.cpp'; src.write_text(replay)
    exe=Path(tmp)/'replay'
    subprocess.run(['g++','-std=c++17','-O2','-I',str(root),str(src),str(root/'third_party/yeney-core/core/protocol.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
