#include "sbencoder.h"
#include "start_lead.h"
#include "position_state.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
extern "C" unsigned get_squeezebox_stream_id() { return 1; }
extern "C" int yeney_is_paused() { return 0; }
extern "C" uint64_t get_sb_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int main(int argc, char** argv) {
    if (argc == 2) { assert(yeney_start_lead_ms() == unsigned(std::strtoul(argv[1],nullptr,10))); std::cout<<"PASS: lead setting "<<yeney_start_lead_ms()<<" ms\n"; return 0; }
    ConnectionPosition position;
    position.reset(1); position.connection(1,1,0); position.pcm(1,1,0,0);
    position.poll(position.token(),1000,2000);
    assert(position.audibleFrames(44100)==44100);
    position.connection(1,2,2200); position.pcm(1,2,132300,2200,true);
    assert(position.frames(44100)==132300 && position.audibleFrames(44100)==44100);
    position.poll(position.token(),0,3500);
    assert(position.audibleFrames(44100)==44100);
    position.poll(position.token(),1000,4000);
    assert(position.audibleFrames(44100)==176400);
    std::cout<<"PASS: resume PCM anchor does not advance audible time until fresh positive Sonos playback\n";
    bridge::SBEncoder encoder(1); assert(encoder.open(24,44100));
    std::atomic<bool> stopped{false}; std::atomic<size_t> delivered{0};
    // Dense incompressible 24-bit opening; no silence or padded 16-bit samples.
    std::vector<char> pcm(2048*6); uint32_t random=123;
    for (auto& byte:pcm) { random=random*1664525u+1013904223u; byte=random>>24; }
    const auto start=get_sb_time_ms();
    auto reader=std::async(std::launch::async,[&] { char bytes[16384]; while(!stopped) {
        int n=encoder.read(bytes,sizeof(bytes),100,false,[&]{return stopped.load();}); if(n>0) delivered+=n;
    }});
    auto producer=std::async(std::launch::async,[&] { while(!stopped) encoder.write(pcm.data(),pcm.size(),0,{},[&]{return stopped.load();}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto burst=encoder.encodedAudioMs(); const auto bytes=delivered.load();
    const auto target=yeney_start_lead_ms() ? yeney_start_lead_ms() : 250;
    assert(burst <= target+350);
    if(yeney_start_lead_ms()) { assert(burst>=1900 && bytes>400000); }
    else { assert(burst>=250 && burst<600 && bytes<150000); }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const auto encoded=encoder.encodedAudioMs();
    const auto elapsed=get_sb_time_ms()-start;
    assert(encoded>burst+850 && encoded<burst+1150);
    assert(encoded<=elapsed+target+47);
    stopped=true; producer.get(); reader.get();
    std::cout<<"PASS: dense 24-bit lead="<<yeney_start_lead_ms()<<" ms initial audio="<<burst<<" ms bytes="<<bytes
             <<"; at wall="<<elapsed<<" ms audio="<<encoded<<" ms; bounded batch and real-time steady pacing\n";
}
