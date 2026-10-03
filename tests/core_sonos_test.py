"""Production sink with a blocking encoder and deterministic audible clock."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = r'''
#include "core_sonos.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
std::atomic<unsigned> id{0}; std::atomic<uint64_t> position{0};
std::atomic<bool> block{false}; std::mutex capturedMutex;
std::vector<char> captured; std::vector<uint64_t> anchors;
extern "C" int sonos_audio_legacy() { return getenv("YENEY_AUDIO") != nullptr; }
extern "C" void yeney_transport(char) {}
extern "C" unsigned get_squeezebox_stream_id() { return id.load(); }
extern "C" void new_squeezebox_stream_id() { ++id; position=0; }
extern "C" void set_squeezebox_audio_rate(unsigned, unsigned) {}
extern "C" uint64_t get_sonos_audible_frames(uint32_t) { return position.load(); }
extern "C" int encode_squeezebox_audio_cancellable(const char* p,int n,uint64_t first,int(*cancel)(void*),void* ctx) {
    while(block && !cancel(ctx)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(cancel(ctx)) return 0;
    std::lock_guard<std::mutex> lock(capturedMutex);
    captured.insert(captured.end(),p,p+n); anchors.push_back(first); return 1;
}
void drain(CoreSonosSink& s) {
    for(int i=0; i<2000 && !s.drained(0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(s.drained(0));
}
int main() {
    CoreSonosSink s; bool legacy=sonos_audio_legacy();
    assert(s.maxSampleRate()==(legacy?44100:48000));
    yeney::Frame f[]={{0x12345600,-0x12345600},{INT32_MAX,INT32_MIN},{0x12340000,-0x12340000}};
    s.command({'s',0,false}); s.trackBoundary(0,{44100,24,2,false},false);
    assert(s.write(f,3)==3); drain(s);
    assert(id==1 && s.audibleFrames()==0);
    unsigned bytes=legacy?2:3;
    std::vector<char> expected;
    for(auto v:f) for(uint32_t x:{uint32_t(v.left),uint32_t(v.right)})
        for(unsigned j=4-bytes;j<4;++j) expected.push_back(char(x>>(8*j)));
    assert(captured==expected);
    position=2; assert(s.audibleFrames()==2);
    s.pause(); position=3; assert(s.audibleFrames()==2 && s.write(f,3)==0);
    s.resume(); assert(s.audibleFrames()==3);
    s.command({'s',0,true}); s.trackBoundary(3,{44100,16,2,false},true);
    assert(s.write(f,3)==3); drain(s);
    assert(id==(legacy?2:1)); assert(anchors.back()==(legacy?0:3));
    s.command({'s',0,true}); s.trackBoundary(6,{48000,24,2,false},true);
    assert(s.write(f,3)==3); drain(s); assert(id==(legacy?3:2));
    position=999999; assert(s.audibleFrames()==9);
    s.flush(); s.stop(); assert(s.audibleFrames()==9);
    s.resume(); block=true;
    s.command({'s',0,false}); s.trackBoundary(9,{44100,16,2,false},false);
    std::vector<yeney::Frame> full(48000,f[0]);
    auto start=std::chrono::steady_clock::now();
    auto count=s.write(full.data(), full.size()); assert(count==48000);
    assert(std::chrono::steady_clock::now()-start < std::chrono::milliseconds(100));
    assert(!s.drained(0)); assert(s.audibleFrames()==9);
    s.stop(); block=false; drain(s); assert(s.audibleFrames()==9);
}
'''
with tempfile.TemporaryDirectory(prefix='yeney-core-sink-') as tmp:
    cpp=Path(tmp)/'sink.cpp'; exe=Path(tmp)/'sink'; cpp.write_text(source)
    subprocess.run(['g++','-std=c++17','-O2','-Wall','-I.','-Ithird_party/yeney-core',str(cpp),'core_sonos.cpp',
                    'core_shm.o','third_party/yeney-core/libyeneycore.a','-lFLAC','-pthread','-o',str(exe)],cwd=root,check=True)
    import os
    for mode in ('24/48','16/44'):
        env={**os.environ}; env.pop('YENEY_AUDIO',None)
        if mode=='16/44':env['YENEY_AUDIO']=mode
        subprocess.run([str(exe)],env=env,check=True)
        print(f'PASS: core sink {mode}: packing, continuity/rate IDs, anchors, buffering/monotonic clock, pause, bounded nonblocking queue, flush and shutdown')
