// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "upnp/encoded_buffer.h"
#include "stream_metrics.h"
#include <cassert>
#include <future>
#include <thread>
#include <iostream>
#include <fstream>
using namespace std::chrono_literals;
int main() {
    upnp::EncodedBuffer slow(3,{},100ms);
    auto writer=std::async(std::launch::async,[&] {
        for(unsigned char n=0;n<80;++n) assert(slow.write(reinterpret_cast<char*>(&n),1)==1);
    });
    assert(writer.wait_for(10ms)==std::future_status::timeout);
    for(unsigned char n=0;n<80;) {
        if(auto p=slow.read()) {
            assert(static_cast<unsigned char>(*p->data)==n++);
            std::this_thread::sleep_for(2ms); slow.freePacket(p);
        } else std::this_thread::yield();
    }
    writer.get();
    assert(!bridge::streamMetrics().overwrites && !bridge::streamMetrics().dropped);
    std::cout<<"PASS encoded slow reader: writer backpressure, FIFO/no lost packets, zero overwrites/drops\n";
    upnp::EncodedBuffer b(1,{},200ms);
    b.write("old",3);
    auto held=b.read(); b.write("queued",6);
    auto pending=std::async(std::launch::async,[&]{return b.write("after-clear",11);});
    assert(pending.wait_for(10ms)==std::future_status::timeout);
    b.clear(); assert(pending.get()==11);
    assert(std::string(held->data,held->size)=="old"); b.freePacket(held);
    auto p=b.read(); assert(std::string(p->data,p->size)=="after-clear"); b.clear();
    assert(std::string(p->data,p->size)=="after-clear"); b.freePacket(p);
    std::cout<<"PASS encoded clear/freePacket: held data survives clear, blocked writer wakes, no stale queued packets\n";
    b.write("x",1); std::atomic<bool> cancel{false};
    auto interrupted=std::async(std::launch::async,[&]{return b.write("y",1,[&]{return cancel.load();});});
    assert(interrupted.wait_for(10ms)==std::future_status::timeout);
    cancel=true; assert(interrupted.wait_for(100ms)==std::future_status::ready); assert(interrupted.get()==0);
    assert(!bridge::streamMetrics().dropped);
    std::cout<<"PASS encoded cancellation: full-buffer writer interrupts without dropping a packet\n";
    unsigned drops=0;
    upnp::EncodedBuffer stalled(2,[&](int capacity){assert(capacity==2); ++drops;},30ms);
    stalled.write("a",1); stalled.write("b",1);
    auto begin=std::chrono::steady_clock::now(); stalled.write("c",1);
    assert(std::chrono::steady_clock::now()-begin>=30ms && drops==1);
    p=stalled.read(); assert(*p->data=='b'); stalled.freePacket(p);
    p=stalled.read(); assert(*p->data=='c'); stalled.freePacket(p);
    assert(bridge::streamMetrics().dropped==1 && !bridge::streamMetrics().overwrites);
    bridge::streamMetrics().configure("stall-fixture","/tmp");
    assert(bridge::streamMetrics().tick(60000));
    unlink(("/tmp/stream-pacing-"+bridge::StreamMetrics::hex("stall-fixture")+".json").c_str());
    std::cout<<"PASS encoded stalled reader: bounded oldest-only drop counted and logged reason=reader-stalled\n";
}
