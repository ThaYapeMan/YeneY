// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "stream_pacing.h"
#include "timing_probe_runtime.h"
#include <cassert>
#include <iostream>
int main(int argc, char **) {
    if (argc > 1) {
        std::cout << (bridge::streamPacingSonos() ? "sonos" : "wall") << ' ' << timing_probe::enabled() << '\n';
        return 0;
    }
    bridge::StreamPacer p;
    assert(p.update(1000,0,{},false)==0);
    auto a=p.update(1010,1000,{0,1.0001,false},false);
    assert(std::abs(a-10.001)<1e-6);
    double previous=a;
    for (uint64_t t=1020;t<30000;t+=10) {
        auto next=p.update(t,1000,{double(t-1000)+200,1.0001,true},false);
        assert(next>=previous && next-previous<=10.102);
        previous=next;
    }
    auto frozen=p.update(30000,1000,{},true);
    assert(p.update(40000,1000,{},true)==frozen);
    auto resumed=p.update(40010,1000,{0,.9999,false},false);
    assert(resumed>frozen && resumed-frozen<10.001);
    auto unlocked=p.update(40020,1000,{0,1.0001,false},false);
    assert(unlocked-resumed<10.002);
    assert(bridge::StreamPacer::ceiling(1000,1000)==2500);
    std::cout << "PASS stream pacing: drift prior, continuous lock/acquisition/loss, monotonic phase slew, pause/resume and bounded admission target\n";
}
