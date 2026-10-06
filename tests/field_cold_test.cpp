// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "timing_probe_runtime.h"
#include "position_state.h"
#include <cassert>
#include <fstream>
#include <regex>
using namespace timing_probe;
static double now;
static double fakeClock() { return now; }
struct Read { double sent, received, second; };
static std::vector<Read> reads() {
    std::ifstream f("tests/fixtures/field-study/cold-journal.txt"); assert(f);
    std::regex r("timing-raw .*send_mono=([0-9.]+) recv_mono=([0-9.]+).*reltime_s=([0-9.]+)");
    std::vector<Read> result; std::string line; std::smatch m;
    while (getline(f,line)) if (regex_search(line,m,r))
        result.push_back({stod(m[1]),stod(m[2]),stod(m[3])});
    assert(result.size()>350); return result;
}
static void recorded(const std::vector<Read>& input) {
    Estimator e; BracketClock original;
    unsigned recovery=0; double locked=NAN;
    for (auto r:input) {
        if (r.second>325) break;
        e.sample(r.sent,r.received,r.second);
        if (e.recovered) { ++recovery; assert(e.edges.empty() && !e.fitted); }
        if (e.decision.observed) {
            auto a=e.decision.edge;
            // Original interval path accepted even the rejected ~1 s brackets.
            original.add({a.second,a.time-a.width/2,a.time+a.width/2});
            if (!e.decision.inlier) assert(e.due <= r.received+.35);
        }
        if (original.locked && !std::isfinite(locked)) {
            locked=r.received-687282.529;
            assert(original.edges.size()>=40 && original.span>=60);
            assert(original.width<=.015 && original.violators<=original.edges.size()*.02);
            printf("PASS: recorded late lock %.3f s: brackets=%zu span=%.0f band_ms=%.3f violations=%u; no gate bypass\n",
                   locked,original.edges.size(),original.span,original.width*1000,original.violators);
        }
    }
    assert(recovery && locked>325 && locked<326);
    puts("PASS: recorded cold rejected edges discard transient predictions and request sub-second search");
}
static void scheduled(const std::vector<Read>& input) {
    // Counterfactual queries cannot be replayed literally: use observed first
    // five midpoint ticks, then the late locked phase/rate inside field brackets.
    std::vector<double> ticks{687282.529};
    for (unsigned n=1;n<=5;++n) {
        for (size_t i=1;i<input.size();++i) if(input[i].second==n && input[i-1].second==n-1) {
            ticks.push_back((input[i-1].sent+input[i].received)/2); break;
        }
    }
    for(unsigned n=6;n<150;++n) ticks.push_back(687282.747+n*1.00001085);
    Diagnostics d(fakeClock); now=687282.529;d.anchor(1,0);d.handed(1,0,44100);
    double locked=NAN; unsigned requests=0;
    for(now=687282.852;now<687282.529+100;now+=.002) {
        auto c=d.start(true,"cold-field-replay");
        if(!d.request(c,now))continue;
        auto r=input[requests++%input.size()]; double rtt=r.received-r.sent;
        double sent=now; now+=rtt;
        double second=std::upper_bound(ticks.begin(),ticks.end(),sent+rtt/2)-ticks.begin()-1;
        d.response(c,true,sent,now,second);
        double t,u;
        if(d.contract(uint64_t(second)*44100,t,u)&&!std::isfinite(locked)) locked=now-687282.529;
    }
    printf("PASS: cold scheduler field counterfactual lock_s=%.3f (original 325.213 s)\n",locked);
    assert(locked>=60 && locked<=65);
}
static void positionBound(const std::vector<Read>& input, bool prior) {
    ConnectionPosition p; p.connection(1,1,687282529);p.pcm(1,1,0,687282529);
    const double scale=prior?1/(1+10.850e-6):1;
    double worst=0; unsigned count=0;
    for(auto r:input) {
        if(r.second<2 || r.second>300)continue;
        double observed=(r.sent+r.received)/2;
        p.poll(p.token(),uint32_t(r.second)*1000,uint64_t(observed*1000));
        // Production reads position frequently, not only at SOAP cadence.
        for(double t=observed;t<observed+.25;t+=.0002) {
            double target=r.second+.5+(t-observed)*scale;
            auto frames=p.smoothFrames(44100,t*1000,false,0,scale,r.second,observed);
            double error=double(frames)/44100-target;
            worst=std::max(worst,std::abs(error));++count;
            assert(std::abs(error)<=.50003);
        }
    }
    // High-frequency steady extrapolation must preserve fractions of frames.
    ConnectionPosition steady;steady.connection(1,1,100000);steady.pcm(1,1,0,100000);
    steady.poll(steady.token(),2000,102500);
    double first=double(steady.smoothFrames(44100,102500,false,0,scale))/44100;
    for(unsigned i=1;i<=100000;++i)steady.smoothFrames(44100,102500+i*.2,false,0,scale);
    double last=double(steady.smoothFrames(44100,122500,false,0,scale))/44100;
    assert(std::abs(last-first-20*scale)<.00003);
    printf("PASS: field pre-lock prior=%d samples=%u interval_error_max_ms=%.3f; high-frequency 20 s drift <1.5 ppm\n",prior,count,worst*1000);
}
static void priorSetting() {
    char directory[]="/tmp/yeney-cold-switch-XXXXXX"; assert(mkdtemp(directory));
    DriftState state; state.speaker("RINCON_switch",directory);
    DriftState::Value value{10.850,1,time(nullptr)},loaded; assert(state.save(value));
    Diagnostics d(fakeClock);d.speaker("RINCON_switch",directory);
    assert(d.priorActive()==priorEnabled());
    assert(state.load(loaded) && loaded.ppm==value.ppm);
    puts("PASS: forced cold start leaves the stored prior intact");
}
int main(int argc,char**){setenv("YENEY_TIMING_PROBE","1",1);if(argc>1){priorSetting();return 0;}auto input=reads();recorded(input);scheduled(input);positionBound(input,false);positionBound(input,true);}
