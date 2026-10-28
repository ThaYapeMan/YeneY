// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_temp.h"
#include "core_queue_sink.h"
#include "wav_writer.h"
#include <cassert>
#include <future>
#include <iostream>
using namespace queue_audio;
int main(int argc, char **) {
    if (argc > 1) {
        Config config; config.root = testTempRoot()+"/podcast-quota";
        const auto start=std::chrono::steady_clock::now();
        {
            Store store("ThreeHours",config);
            const uint64_t frames=3ull*60*60*44100;
            auto item=store.create({"three-hours",44100,frames+5*44100,0,2,24,PromiseKind::UpperBound});
            store.protect(item,{});
            std::vector<yeney::Frame> data(4096);
            uint32_t random=1;
            auto sample=[&] { random^=random<<13;random^=random>>17;random^=random<<5;return int32_t(random&0xffffff00); };
            for(uint64_t n=0;n<frames;) {
                for(auto &f:data) f={sample(),sample()};
                const auto count=std::min<uint64_t>(frames-n,data.size());
                item->append(data.data(),count,false);n+=count;
            }
            item->finish();
            char dataAtStart[6],dataAtEnd[6];
            assert(item->reader(44).read(dataAtStart,6)==6);
            assert(item->reader(44+(frames-1)*6).read(dataAtEnd,6)==6);
            std::cout<<"PASS 3-hour 44.1 kHz incompressible episode: frames="<<frames<<" PCM_bytes="<<frames*6
                     <<" disk_bytes="<<store.diskBytes()<<" RAM_bytes="<<store.ramBytes()<<" quota_bytes="<<config.estimatedDiskBytes
                     <<" elapsed_s="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<"; first and last frames retained\n";
        }
        std::filesystem::remove_all(config.root);
        return 0;
    }
    auto root = testTempRoot() + "/podcast-store";
    Config c; c.root = root; c.ramBytes = 24576;
    CoreQueueSink sink("Podcast", c, 60);
    sink.useSpeakerClock();
    sink.estimate(1, 20, 4000);
    sink.coreEvent("strm", 1, 0);
    sink.trackDescriptor({1, 8000, {}, 0, 0});
    sink.trackBoundary(0, {8000, 24, 2, false}, false);
    sink.waitIdle();
    auto records = sink.snapshot();
    assert(records.size()==1 && records[0].promised==21*8000);
    assert(records[0].item->descriptor().kind==PromiseKind::UpperBound);
    auto future = std::async(std::launch::async,[item=records[0].item] {
        char data[6]; return item->reader(44+100000*6).read(data,6);
    });
    std::vector<yeney::Frame> pcm(46*8000);
    for (size_t i=0;i<pcm.size();++i) pcm[i]={int32_t(i<<8),-int32_t(i<<8)};
    assert(sink.write(pcm.data(), pcm.size(), false)==pcm.size());
    sink.resolvedItemBoundary({1,0,pcm.size(),0,"no-transition"});
    sink.waitIdle();
    assert(future.get()==6);
    records=sink.snapshot();
    assert(records.size()>=2);
    assert(records.back().item->descriptor().kind==PromiseKind::Exact);
    std::vector<char> actual;
    for (const auto &r:records) {
        assert(r.item->finished());
        assert(r.item->contentLength()==wavLength(*r.promised));
        auto reader=r.item->reader(44);
        char bytes[8192];
        while(auto n=reader.read(bytes,sizeof bytes)) actual.insert(actual.end(),bytes,bytes+n);
    }
    assert(actual.size()==pcm.size()*6);
    for (size_t i=0;i<pcm.size();++i) for(unsigned b=0;b<3;++b) {
        assert(uint8_t(actual[i*6+b])==uint8_t(uint32_t(pcm[i].left)>>(8+b*8)));
        assert(uint8_t(actual[i*6+3+b])==uint8_t(uint32_t(pcm[i].right)>>(8+b*8)));
    }
    sink.speakerPosition(1,20*8000,yeney::PlaybackState::Playing);
    yeney::AudibleStart start; assert(sink.takeAudibleStart(start) && start.occurrence==1);
    sink.speakerPosition(records[1].source.occurrence,2*8000,yeney::PlaybackState::Playing);
    assert(!sink.takeAudibleStart(start));
    assert(sink.audibleElapsedMs(1)==23000);
    sink.speakerPosition(records[1].source.occurrence,8000,yeney::PlaybackState::Playing);
    assert(sink.audibleElapsedMs(1)==22000);
    sink.restartCached();
    sink.speakerPosition(records[1].source.occurrence,8000,yeney::PlaybackState::Playing);
    assert(sink.takeAudibleStart(start) && start.occurrence==1 && start.frame==records[0].first);
    assert(sink.audibleElapsedMs(1)==22000);
    assert(sink.queueItemProduced(1));
    assert(sink.committedItemFrames(1)==pcm.size());
    std::cout<<"PASS podcast: resumed declaration ceil((20-4)*8000)+5*8000; sample-exact 46 s with an exact completed continuation; one LMS start and continuous seek clock\n";
    Store store("Short",c);
    auto shortItem=store.create({"short",8000,80000,0,2,24,PromiseKind::UpperBound});
    shortItem->append(pcm.data(),40000,false);shortItem->finish();
    char bytes[6]; assert(shortItem->reader(44+40000*6).read(bytes,6)==0);
    assert(shortItem->reader(44+50000*6).read(bytes,6)==0);
    auto pending=store.create({"waiting",8000,80000,0,2,24,PromiseKind::UpperBound});
    bool timeout=false;
    try { pending->reader(44+60000*6).read(bytes,6,std::chrono::milliseconds(5)); }
    catch(const ReadTimeout &) { timeout=true; }
    assert(timeout);
    // An arbitrary pause consumes no store data and changes no immutable bytes.
    auto before=shortItem->reader(44+30000*6);auto after=shortItem->reader(44+30000*6);
    char a[6],b[6];assert(before.read(a,6)==6 && after.read(b,6)==6);
    assert(std::equal(a,a+6,b));
    std::cout<<"PASS podcast: short EOF normal; future Range waits and times out; immutable spool survives pause\n";
}
