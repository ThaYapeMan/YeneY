"""Deterministic response cadence and opt-in tests (no 50-second sleep)."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include "stream_debug.h"
#include <cassert>
using Clock=std::chrono::steady_clock;
static Clock::time_point tick;
struct Request: upnp::StreamRequest {
    uint64_t bytes=0;
    std::string path() const override {return "/music/yeney.flac";}
    Method method() const override {return Method::Get;}
    std::string parameter(const std::string&) const override {return "3";}
    std::string serverName() const override {return "fixture";}
    bool send(const char*,size_t n) override {bytes+=n;tick+=std::chrono::milliseconds(2);return true;}
    uint64_t connectionSentBytes() const override {return bytes;}
    std::string connectionDiagnostics() const override {return "rtt_us=10 rttvar_us=2 snd_cwnd=10 unacked=1 retransmits=0 total_retrans=0 end=peer_FIN";}
    bool peerClosed() override {return false;}
    void sendTimeout(unsigned) override {}
    void disconnect() override {}
    void reply(unsigned,const std::string&) override {}
};
int main() {
    Request wire; StreamDebugRequest request(wire,[]{return tick;});
    request.encoder([]{return uint64_t(2000+std::chrono::duration_cast<std::chrono::milliseconds>(tick.time_since_epoch()).count());});
    assert(request.send("head",4));
    for(int second=1;second<=50;++second) {
        tick=Clock::time_point(std::chrono::seconds(second)); request.peerClosed();
        assert(request.send("pcm",3));
    }
}
'''
with tempfile.TemporaryDirectory(prefix='yeney-stream-debug-') as tmp:
    cpp=Path(tmp)/'test.cpp';cpp.write_text(source)
    binary=Path(tmp)/'test'
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(root),str(cpp),'-o',str(binary)],check=True)
    for setting in (None,'true','1'):
        env=dict(os.environ);env.pop('YENEY_DEBUG_STREAM',None)
        if setting is not None:env['YENEY_DEBUG_STREAM']=setting
        run=subprocess.run([str(binary)],env=env,capture_output=True,text=True,check=True)
        lines=run.stdout.splitlines()
        if setting!='1':assert not lines
        else:
            periodic=[line for line in lines if not line.endswith(' final')]
            assert [float(line.split('t=')[1].split()[0]) for line in periodic]==list(range(1,31))+[40,50],lines
            assert 'bytes=4 ' in periodic[0] and 'send_max_ms=2.000' in periodic[0],lines
            assert 'bytes=30 ' in periodic[-1],lines
            assert 'end=peer_FIN final' in lines[-1],lines
        print(f'PASS: YENEY_DEBUG_STREAM={setting!r}: response cadence 1..30 then 40/50, interval wire bytes, send durations, final termination',flush=True)
