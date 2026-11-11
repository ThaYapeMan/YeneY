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
            sink.contentPosition(origin,true);
            sink.trackBoundary(0,{8000,24,2,false},false);
            assert(sink.write(pcm.data(),pcm.size(),true)==pcm.size());
            sink.resolvedItemBoundary({1,0,5000,0,"no-transition"}); sink.waitIdle();
            auto original=sink.snapshot()[0].item;
            auto read=[](auto item) {
                std::vector<char> bytes(*item->contentLength()); auto reader=item->reader(0);
                size_t at=0;
                while(at<bytes.size()) { auto n=reader.read(bytes.data()+at,bytes.size()-at); assert(n); at+=n; }
                return bytes;
            };
            auto before=read(original);
            sink.retainInterruptedOnNextFlush(); sink.flush();
            sink.sourceIdentity(2,7,"file://fixture",63);
            sink.coreEvent("strm",2,0);
            sink.trackDescriptor({2,8000,4500,0,0});
            sink.contentPosition(origin+500,true);
            sink.trackBoundary(5000,{8000,24,2,false},false);
            sink.waitIdle();
            if(mode=="mismatch") pcm[623].left ^= 65536;
            assert(sink.write(pcm.data()+500,4500,true)==4500);
            sink.resolvedItemBoundary({2,0,9500,0,"no-transition"}); sink.waitIdle();
            auto row=sink.snapshot()[1];
            assert(read(original)==before);
            if(mode=="mismatch") {
                assert(row.item!=original && row.seekReason=="landed-pcm-mismatch");
                std::cout << "PASS D1 exact overlap mismatch is fallback; retained WAV readable and byte-identical; worker alive\n";
            } else {
                assert(row.item==original && row.actual==5000);
                std::cout << "PASS D4 source origin 113; original landing 613; WAV offset 500; exact immutable overlap\n";
            }
        }
    }
    std::filesystem::remove_all(directory);
}
