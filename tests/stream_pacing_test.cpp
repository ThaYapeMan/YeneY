// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "stream_pacing.h"
#include "timing_probe_runtime.h"
#include <cassert>
#include <iostream>
#include "upnp/encoded_buffer.h"
#include "stream_metrics.h"
#include <vector>
#include <cstring>
// Virtual 1 ms transport packets deliberately stress the packet-slot capacity;
// the device/socket reservoir is separate from the 256 unsent host slots.
// This is an audio-clock/transport simulation, not a FLAC codec benchmark.
struct LegacyRing {
    std::vector<bool> slots = std::vector<bool>(256);
    size_t writer=0, reader=0, queued=0;
    uint64_t overwrites=0;
    void write() {
        if (slots[writer]) ++overwrites; else ++queued;
        slots[writer]=true; writer=(writer+1)%slots.size();
    }
    bool read() {
        if (!queued) return false;
        while(!slots[reader]) reader=(reader+1)%slots.size();
        slots[reader]=false; reader=(reader+1)%slots.size(); --queued; return true;
    }
};
static void hours(bool sonos, int ppm, unsigned lead) {
    bridge::StreamPacer pacer;
    upnp::EncodedBuffer ring(256);
    LegacyRing legacy;
    constexpr uint64_t duration=4ull*3600*1000;
    double rate=1+ppm*1e-6, sent=0, low=1e100, high=-1e100;
    uint64_t encoded=0, expected=0;
    bool underrun=false;
    const double socketCapacity=lead+500;
    for (uint64_t t=0; t<=duration; ++t) {
        double audible=t*rate;
        bool locked=t>=60000 && !(t>=7200000 && t<7260000); // acquire, lose, reacquire
        // Prior is deliberately unity: lock must correct real +/-100 ppm drift.
        double played=sonos ? pacer.update(t+1,1,{audible, locked ? rate : 1.,locked},false) : double(t);
        double limit=sonos ? bridge::StreamPacer::ceiling(played,lead) : played+(lead ? lead : 250);
        auto drain=[&] {
            while(sent-audible<socketCapacity) {
                if (sonos) {
                    auto p=ring.read(); if(!p) break;
                    uint64_t sequence; std::memcpy(&sequence,p->data,sizeof sequence);
                    assert(sequence==expected++); ring.freePacket(p);
                } else if (!legacy.read()) break;
                sent+=1;
            }
        };
        drain();
        while(double(encoded+1)<=limit) {
            if (sonos) assert(ring.write(reinterpret_cast<const char*>(&encoded),sizeof encoded)==sizeof encoded);
            else legacy.write();
            ++encoded; drain();
        }
        double current=double(encoded)-audible;
        low=std::min(low,current); high=std::max(high,current);
        underrun=underrun || sent+1e-6<audible;
    }
    if(sonos) {
        assert(low>=lead && high<=lead+1000 && !underrun);
        assert(bridge::streamMetrics().overwrites==0 && bridge::streamMetrics().dropped==0);
        std::cout<<"PASS Sonos pacing 4 simulated hours ppm="<<ppm<<" start_lead_ms="<<lead
                 <<" lead_min_ms="<<low<<" lead_max_ms="<<high<<" overwrites=0 drops=0; lock/loss/relock continuous\n";
    } else {
        if(ppm>0) assert(underrun);
        if(ppm<0) assert(legacy.overwrites>0);
        if(!ppm) assert(!underrun && !legacy.overwrites);
        std::cout<<"PASS legacy wall 4 simulated hours ppm="<<ppm<<" start_lead_ms="<<lead
                 <<" lead_min_ms="<<low<<" lead_max_ms="<<high<<" overwrites="<<legacy.overwrites
                 <<" underrun="<<underrun<<"; old drift failure reproduced\n";
    }
}
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
    for(unsigned lead : {0u,1000u}) for(int ppm : {100,0,-100}) hours(true,ppm,lead);
    for(int ppm : {100,0,-100}) hours(false,ppm,0);
    std::cout << "PASS stream pacing: drift prior, continuous lock/acquisition/loss, monotonic phase slew, pause/resume and bounded admission target\n";
}
