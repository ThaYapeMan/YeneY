"""Replay S7#2 through ConnectionPosition, output correction and actual STAT bytes."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
stat = r'''
#include <stdint.h>
static uint32_t clock_ms;
static uint32_t replay_now(void) { return clock_ms; }
#define gettime_ms replay_now
#include "slimproto_sonos.c"
#undef gettime_ms
#include <assert.h>
static int peer;
void report_init(void) {
    int fds[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sock = fds[0]; peer = fds[1];
    unsetenv("DISABLE_SONOS_POSITION_FIX");
}
u32_t report(u32_t now, u32_t rate, u32_t frames, u32_t device, u32_t epoch) {
    clock_ms = now;
    status.current_sample_rate = rate; status.frames_played = frames;
    status.device_frames = device; status.stream_start = epoch;
    status.updated = now - 1500; // a blocked producer must not advance the clock
    sendSTAT("STMt", 0);
    struct STAT_packet pkt;
    assert(recv(peer, &pkt, sizeof(pkt), MSG_WAITALL) == sizeof(pkt));
    assert(!memcmp(pkt.opcode, "STAT", 4));
    u32_t ms = unpackN(&pkt.elapsed_milliseconds);
    assert(unpackN(&pkt.elapsed_seconds) == ms / 1000);
    return ms;
}
void report_close(void) { close(sock); close(peer); }
'''
output = r'''
#include "output_sonos.c"
struct outputstate output;
int sonos_audio_legacy(void) { return 0; }
u32_t replay_device(u32_t rate, u32_t decoded) {
    output.current_sample_rate = rate; output.frames_played_dmp = decoded;
    update_device_frames_from_sonos_position();
    return output.device_frames;
}
'''
replay = r'''
#include "position_state.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
static ConnectionPosition position;
extern "C" {
uint64_t get_sonos_audible_frames(uint32_t rate) { return position.audibleFrames(rate); }
void report_init();
void report_close();
uint32_t replay_device(uint32_t rate, uint32_t decoded);
uint32_t report(uint32_t now, uint32_t rate, uint32_t decoded, uint32_t device, uint32_t epoch);
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
    tmp = Path(tmp)
    objects = []
    for name, source in [('stat', stat), ('output', output)]:
        src = tmp / (name + '.c'); src.write_text(source)
        obj = tmp / (name + '.o'); objects.append(str(obj))
        subprocess.run(['gcc', '-std=gnu11', '-O2', '-ffunction-sections', '-fdata-sections',
                        '-I'+str(root), '-I'+str(root/'squeezelite'), '-c', str(src), '-o', str(obj)], check=True)
    obj = tmp / 'utils.o'; objects.append(str(obj))
    subprocess.run(['gcc', '-O2', '-ffunction-sections', '-fdata-sections', '-c',
                    str(root/'squeezelite/utils.c'), '-o', str(obj)], check=True)
    src = tmp / 'replay.cpp'; src.write_text(replay)
    exe = tmp / 'replay'
    subprocess.run(['g++', '-O2', '-I'+str(root), str(src), *objects,
                    '-Wl,--gc-sections', '-lpthread', '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
