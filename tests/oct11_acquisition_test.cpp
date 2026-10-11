// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "timing_probe_runtime.h"
#include "position_state.h"
#include <cassert>
#include <fstream>
#include <regex>
using namespace timing_probe;
static double now;
static double clockNow() { return now; }
static void replay(bool prior) {
    std::ifstream f("tests/fixtures/field-2026-10-11/journal.txt"); assert(f);
    std::regex raw("stream=2 .*send_mono=([0-9.]+) recv_mono=([0-9.]+).*reltime_s=([0-9.]+)");
    std::regex edge("stream=2 .*second=[0-9]+->([0-9]+) lo_mono=([0-9.]+) hi_mono=([0-9.]+)");
    struct Read { double sent, received, second; };
    std::vector<Read> input;
    std::string line; std::smatch m; std::vector<double> rtts; BracketClock recorded;
    double zero=0, literal=NAN;
    if (prior) recorded.usePrior(8.350,6.928);
    while(getline(f,line)) {
        if(line.find(prior ? "[43848]" : "[43745]")==std::string::npos) continue;
        if(regex_search(line,m,raw)) {
            rtts.push_back(stod(m[2])-stod(m[1]));
            input.push_back({stod(m[1]),stod(m[2]),stod(m[3])});
        }
        if(regex_search(line,m,edge)) {
            recorded.add({stod(m[1]),stod(m[2]),stod(m[3])});
            if(recorded.locked && !std::isfinite(literal)) literal=recorded.lastTwoSided;
        }
    }
    assert(recorded.locked && rtts.size()>100);
    zero=recorded.timeAt(0);
    double start=prior ? 1110750.554 : 1110013.688;
    char directory[]="/tmp/yeney-oct11-XXXXXX";assert(mkdtemp(directory));
    DriftState state;state.speaker("oct11",directory);
    if(prior) assert(state.save({8.350,6.928,time(nullptr)}));
    now=start; Diagnostics literalRuntime(clockNow);
    literalRuntime.speaker("oct11",directory);literalRuntime.anchor(2,0);literalRuntime.handed(2,0,48000);
    auto literalContext=literalRuntime.start(true,"oct11-literal");
    double literalLock=NAN;
    for(auto r:input) {
        if(r.received>start+90) break;
        now=r.received;literalRuntime.response(literalContext,true,r.sent,r.received,r.second);
        double t,u;
        if(literalRuntime.contract(uint64_t(r.second)*48000,t,u)) { literalLock=now-start;break; }
    }
    assert(std::isfinite(literalLock) && literalLock <= (prior?42:68));
    printf("October 11 literal runtime prior=%d lock_s=%.3f\n",prior,literalLock);
    now=start; Diagnostics d(clockNow); d.speaker("oct11",directory); d.anchor(2,0);d.handed(2,0,48000);
    double locked=NAN;unsigned requests=0;
    // Counterfactual scheduling: final field-supported phase/rate and captured
    // RTT sequence. New query times cannot have literal recorded responses.
    for(now=start;now<start+90;now+=.002) {
        auto c=d.start(true,"oct11");if(!d.request(c,now))continue;
        double sent=now,rtt=rtts[requests++%rtts.size()];now+=rtt;
        double n=std::max(0.,std::floor(((sent+now)/2-zero)/(1+recorded.drift*1e-6)));
        d.response(c,true,sent,now,n);
        if (n > 0) assert(d.heardAudio(c.stream));
        else { double zeroTime; assert(d.startupZero(c.stream,zeroTime)); }
        double t,u;if(d.contract(uint64_t(n)*48000,t,u)&&!std::isfinite(locked))locked=now-start;
    }
    printf("October 11 prior=%d recorded_first_band_s=%.3f scheduled_lock_s=%.3f\n",prior,literal-start,locked);
    assert(std::isfinite(locked) && locked <= (prior?15:63));
}
static void carriedDrift() {
    now=100; Diagnostics d(clockNow);d.anchor(2,0);d.handed(2,0,48000);
    auto c=d.start(true,"rate-change");
    for(unsigned n=1;n<=175;++n) {
        double tick=100+n*1.000009;
        now=tick-.0005;d.response(c,true,now-.00025,now+.00025,n-1);
        now=tick+.0005;d.response(c,true,now-.00025,now+.00025,n);
    }
    assert(!d.priorActive()); // live drift has not met disk persistence age
    d.reset("stream-reset",true);
    assert(d.priorActive());
    std::ifstream field("tests/fixtures/field-2026-10-11/journal.txt"); assert(field);
    std::regex raw("stream=3 .*send_mono=([0-9.]+) recv_mono=([0-9.]+).*reltime_s=([0-9.]+)");
    std::regex edge("stream=3 .*second=[0-9]+->([0-9]+) lo_mono=([0-9.]+) hi_mono=([0-9.]+)");
    BracketClock recorded; recorded.usePrior(9,7);
    std::vector<double> rtts; std::string line; std::smatch m;
    while(getline(field,line)) {
        if(line.find("[43745]")==std::string::npos) continue;
        if(regex_search(line,m,raw)) rtts.push_back(stod(m[2])-stod(m[1]));
        if(regex_search(line,m,edge)) recorded.add({stod(m[1]),stod(m[2]),stod(m[3])});
    }
    assert(recorded.fitted && rtts.size()>20);
    double start=1110603.844,locked=NAN;unsigned requests=0;
    now=start;d.anchor(3,0);d.handed(3,0,44100);
    assert(!d.heardAudio(3));
    double zeroTime; assert(!d.startupZero(3,zeroTime));
    for(now=start;now<start+25;now+=.002) {
        c=d.start(true,"rate-change");if(!d.request(c,now))continue;
        double sent=now;now+=rtts[requests++%rtts.size()];
        double n=std::max(0.,std::floor(((sent+now)/2-recorded.timeAt(0))/(1+recorded.drift*1e-6)));
        d.response(c,true,sent,now,n);
        if (n > 0) assert(d.heardAudio(c.stream));
        else { double zeroTime; assert(d.startupZero(c.stream,zeroTime)); }
        double t,u;if(d.contract(uint64_t(n)*44100,t,u)&&!std::isfinite(locked))locked=now-start;
    }
    assert(std::isfinite(locked)&&locked<=15);
    printf("PASS: 48 to 44.1 kHz retains unsaved live drift, reacquires phase in %.3f s\n",locked);
}
static void rateChangePosition() {
    ConnectionPosition p; p.reset(3);p.allowZeroStart();p.reset(3);p.connection(3,1,100000);p.pcm(3,1,0,100000);
    p.startZero(101200); // fresh probe zero, without waiting for the normal lease
    p.poll(p.token(),0,101200);
    assert(!p.audioStarted());
    double previous=0;
    for(unsigned i=0;i<40;++i) {
        double t=101200+i*250;
        unsigned ms=unsigned((t-101200)/1000)*1000;
        p.poll(p.token(),ms,uint64_t(t));
        assert(p.audioStarted() == (ms > 0));
        double next=double(p.smoothFrames(44100,t,false,0))/44100;
        if(i) assert(next>previous && next-previous<.251);
        previous=next;
    }
    puts("PASS: 44.1 kHz new-stream zero anchor advances at unity without frozen floor or 2.5 s jump");
}
int main(){rateChangePosition();setenv("YENEY_TIMING_PROBE","1",1);replay(false);replay(true);carriedDrift();}
