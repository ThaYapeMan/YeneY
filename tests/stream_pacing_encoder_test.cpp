// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sbencoder.h"
#include "stream_metrics.h"
#include <cassert>
#include <future>
#include <thread>
#include <vector>
#include <iostream>
std::atomic<uint64_t> clockMs{1000};
extern "C" uint64_t get_sb_time_ms() { return clockMs; }
extern "C" unsigned get_squeezebox_stream_id() { return 1; }
extern "C" int yeney_is_paused() { return 0; }
namespace bridge {
struct EncoderTestAccess {
    static double accepted(const SBEncoder &e) { return e.m_pcmBytesAccepted.load()/6.*1000/44100; }
};
}
int main() {
    assert(bridge::streamPacingSonos());
    bridge::SBEncoder encoder(1); assert(encoder.open(24,44100));
    encoder.pacingClock([] { auto t=clockMs.load()-1000; return bridge::StreamClock{t*1.0001,1.0001,t>=500}; });
    std::vector<char> pcm(44100*6*5,0);
    std::atomic<bool> stop{false};
    auto reader=std::async(std::launch::async,[&] {
        char data[4096]; while(!stop) encoder.read(data,sizeof data,20,false,[&]{return stop.load();});
    });
    auto writer=std::async(std::launch::async,[&] {
        return encoder.write(pcm.data(),pcm.size(),0,{},[&]{return stop.load();});
    });
    for(unsigned t=0;t<=3000;t+=50) {
        clockMs=1000+t;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(bridge::EncoderTestAccess::accepted(encoder)<t+1400 && std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        double accepted=bridge::EncoderTestAccess::accepted(encoder);
        assert(accepted>=t+1400 && accepted<=t*1.0001+1501);
        // The emitted FLAC clock trails admission by at most one buffered frame.
        auto encoded=encoder.encodedAudioMs();
        assert(double(encoded)-t>=1000 && double(encoded)-t<=2000);
    }
    stop=true; encoder.cancel(); writer.get(); reader.get();
    assert(!bridge::streamMetrics().overwrites && !bridge::streamMetrics().dropped);
    std::cout<<"PASS Sonos pacing real encoder: frame-sized admission, drift-fit mapping, bounded encoded lead, full-buffer cancellation, zero overwrites/drops\n";
}
