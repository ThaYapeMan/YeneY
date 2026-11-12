// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include "test_temp.h"
#include <cassert>
#include <filesystem>
#include <iostream>
using namespace queue_audio;
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    auto pattern = testTempRoot() + "/seek-worker-XXXXXX";
    auto directory = mkdtemp(pattern.data());
    assert(directory);
    Config config; config.root = directory;
    {
        CoreQueueSink sink("Test", config, 1), healthy("Healthy", config, 1);
        if (mode == "worker") {
            // This fixture alone is compiled with -fno-access-control. It forces
            // a programming fault in the real worker without a runtime hook.
            sink.enqueue([] { throw std::logic_error("injected programming error"); });
            sink.waitIdle();
            assert(sink.workerFault == "injected programming error");
            healthy.sourceIdentity(1, 9, "file://healthy-room", 0);
            healthy.coreEvent("strm", 1, 0);
            healthy.trackDescriptor({1, 8000, 100, 0, 0});
            healthy.trackBoundary(0, {8000, 24, 2, false}, false);
            std::vector<yeney::Frame> pcm(100, {65536, -65536});
            assert(healthy.write(pcm.data(), pcm.size(), false) == pcm.size());
            healthy.resolvedItemBoundary({1, 0, 100, 0, "no-transition"});
            healthy.waitIdle();
            auto item = healthy.snapshot()[0].item;
            assert(healthy.workerFault.empty() && item->finished() && item->producedFrames() == 100);
            char bytes[6];
            assert(item->reader(44).read(bytes, sizeof(bytes)) == sizeof(bytes));
            std::cout << "PASS D6 worker fault contained; stopped worker; process alive; second room produces readable PCM\n";
        } else {
            const uint64_t origin = mode == "trim" ? 113 : 0;
            std::vector<yeney::Frame> pcm;
            for (int n=0; n<5000; ++n) pcm.push_back({n*65536, -n*65536});
            sink.windowRetention(2,3);
            sink.sourceIdentity(1,7,"file://fixture",0);
            sink.coreEvent("strm",1,0);
            sink.trackDescriptor({1,8000,5000,0,0});
            sink.contentPosition(origin,mode != "unknown-origin");
            sink.trackBoundary(0,{8000,24,2,false},false);
            const size_t prefix = mode == "warmup-short" ? 4596 : pcm.size();
            assert(sink.write(pcm.data(),prefix,true)==prefix);
            if(mode != "warmup-short") sink.resolvedItemBoundary({1,0,5000,0,"no-transition"});
            sink.waitIdle();
            auto original=sink.snapshot()[0].item;
            auto read=[](auto item) {
                std::vector<char> bytes(44+item->producedFrames()*6); auto reader=item->reader(0);
                size_t at=0;
                while(at<bytes.size()) { auto n=reader.read(bytes.data()+at,bytes.size()-at); assert(n); at+=n; }
                return bytes;
            };
            auto before=read(original);
            if(mode=="unknown-origin") {
                sink.retainInterruptedOnNextFlush(); sink.flush();
                sink.sourceIdentity(2,7,"file://fixture",0);
                sink.coreEvent("strm",2,0);
                sink.trackDescriptor({2,8000,5000,0,0});
                sink.contentPosition(0,true,128);
                sink.trackBoundary(5000,{8000,24,2,false},false);
                assert(sink.write(pcm.data(),pcm.size(),true)==pcm.size());
                sink.resolvedItemBoundary({2,0,10000,0,"no-transition"}); sink.waitIdle();
                assert(sink.snapshot().back().pairedFrom==1);
            }
            const uint64_t sought = mode=="unknown-origin" ? 3 : 2;
            sink.retainInterruptedOnNextFlush(); sink.flush();
            sink.sourceIdentity(sought,7,"file://fixture",63);
            sink.coreEvent("strm",sought,0);
            sink.trackDescriptor({sought,8000,4500,0,0});
            sink.contentPosition(origin+500, mode != "unknown", (mode == "warmup" || mode == "warmup-short" || mode == "unknown-origin") ? 128 : 0);
            const uint64_t seekFirst = sought==3 ? 10000 : 5000;
            sink.trackBoundary(seekFirst,{8000,24,2,false},false);
            sink.waitIdle();
            if(mode=="mismatch") pcm[623].left ^= 65536;
            if(mode=="warmup" || mode=="warmup-short") for(size_t i=500; i<628; ++i) pcm[i].left ^= 65536;
            assert(sink.write(pcm.data()+500,4500,true)==4500);
            sink.resolvedItemBoundary({sought,0,seekFirst+4500,0,"no-transition"}); sink.waitIdle();
            auto row=sink.snapshot().back();
            assert(read(original)==before);
            if(mode=="warmup-short") {
                assert(row.item!=original && row.seekReason=="prefix-unavailable");
                std::cout << "PASS converted prefix must include settling and the full 4096-frame overlap\n";
            } else if(mode=="unknown" || mode=="unknown-origin") {
                assert(row.item!=original && row.seekReason=="landed-frame-unknown");
                assert(sink.workerFault.empty());
                if(mode=="unknown-origin") std::cout << "PASS unknown original WAV coordinate stays unknown through frame-zero aliases\n";
                else std::cout << "PASS E6 unprovable converted landing falls back without worker fault\n";
            } else if(mode=="mismatch") {
                assert(row.item!=original && row.seekReason=="landed-pcm-mismatch");
                std::cout << "PASS D1 exact overlap mismatch is fallback; retained WAV readable and byte-identical; worker alive\n";
            } else {
                assert(row.item==original && row.actual==5000);
                if(mode=="warmup") {
                    assert(row.attachOffset==628 && row.warmupRemaining==0 && row.warmupFrames==128);
                    assert(sink.committedItemFrames(2)==4500);
                    std::cout << "PASS E2 warmup discarded; 4096-frame overlap; clock frame budget includes settling\n";
                } else std::cout << "PASS D4 source origin 113; original landing 613; WAV offset 500; exact immutable overlap\n";
            }
        }
    }
    std::filesystem::remove_all(directory);
}
