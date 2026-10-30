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
    for(unsigned seconds:{80u,320u,50u,100u}) {
        Config cfg;cfg.root=testTempRoot()+"/podcast-decisions";
        CoreQueueSink trial("Case"+std::to_string(seconds),cfg,600);trial.useSpeakerClock();
        trial.windowRetention(2,3);
        trial.estimate(1,20,0);trial.coreEvent("strm",1,0);
        trial.trackDescriptor({1,8000,{},0,0});trial.trackBoundary(0,{8000,24,2,false},false);
        std::vector<yeney::Frame> audio(seconds*8000);
        for(size_t i=0;i<audio.size();++i)audio[i]={int32_t(i<<8),-int32_t(i<<8)};
        const bool forced=seconds==50 || seconds==100;
        const size_t first=forced?30*8000:audio.size();
        assert(trial.write(audio.data(),first,false)==first);trial.waitIdle();
        auto parts=trial.snapshot();assert(parts.size()==2 && !parts.back().promised);
        const auto pendingSnapshot=parts.back().item;
        trial.speakerPosition(1,14*8000,yeney::PlaybackState::Playing);trial.waitIdle();
        assert(!trial.snapshot().back().promised);
        if(forced) {
            trial.speakerPosition(1,15*8000,yeney::PlaybackState::Playing);trial.waitIdle();
            parts=trial.snapshot();assert(parts.size()==2 && parts.back().promised==30*8000);
            assert(parts.back().continuationDecision=="deadline-forced");
            assert(parts.back().item->descriptor().kind==PromiseKind::UpperBound);
            assert(trial.write(audio.data()+first,audio.size()-first,false)==audio.size()-first);
        }
        trial.resolvedItemBoundary({1,0,audio.size(),0,"no-transition"});trial.waitIdle();
        parts=trial.snapshot();assert(parts.size()==(seconds==100?3u:2u));
        if(seconds==80) {
            std::optional<uint64_t> staleLength;
            try { staleLength=pendingSnapshot->contentLength(); }
            catch(const PublicationSuperseded &) {}
            assert(!staleLength && parts.back().item!=pendingSnapshot);
            assert(parts.back().item->contentLength()==wavLength(*parts.back().promised));
            std::cout<<"PASS pending continuation snapshot: stale private snapshot has no usable header; latest immutable promise remains available\n";
        }
        if(!forced) {
            assert(parts.back().promised==(seconds-25)*8000);
            assert(parts.back().continuationDecision=="exact");
            assert(parts.back().item->descriptor().kind==PromiseKind::Exact);
        }
        if(seconds==100)assert(parts.back().continuationDecision=="bound-exceeded");
        std::vector<char> bytes;
        for(const auto &part:parts) {
            assert(part.item->finished());
            auto reader=part.item->reader(44);char chunk[8192];
            while(auto n=reader.read(chunk,sizeof chunk))bytes.insert(bytes.end(),chunk,chunk+n);
        }
        assert(bytes.size()==audio.size()*6);
        for(size_t i=0;i<audio.size();++i)for(unsigned b=0;b<3;++b) {
            assert(uint8_t(bytes[i*6+b])==uint8_t(uint32_t(audio[i].left)>>(8+b*8)));
            assert(uint8_t(bytes[i*6+3+b])==uint8_t(uint32_t(audio[i].right)>>(8+b*8)));
        }
        yeney::AudibleStart start;assert(trial.takeAudibleStart(start) && start.occurrence==1);
        for(size_t i=1;i<parts.size();++i) {
            trial.speakerPosition(parts[i].source.occurrence,parts[i].item->producedFrames(),yeney::PlaybackState::Playing);
            assert(!trial.takeAudibleStart(start));
        }
        std::cout<<"PASS continuation decision case="<<seconds<<"s: "
                 <<(!forced?"one exact remainder, 11 s lead, LMS underestimate "+std::to_string(seconds-20)+" s":
                    seconds==50?"one deadline-forced bound at 10 s lead, 5 s overflow + 25 s parent, clean early EOF":
                    "deadline-forced bound then one bound-exceeded exact remainder")
                 <<"; sample-exact concatenation, one LMS start, continuous clock\n";
        trial.releaseParent(1);trial.waitIdle();
        for (const auto &part:parts) {
            bool released=false;
            try { char byte;part.item->reader(44).read(&byte,1); }
            catch (const ItemCancelled &) { released=true; }
            assert(released);
        }
        for (const auto &record:trial.snapshot()) assert(record.incomplete=="outside queue window");
        std::cout<<"PASS continuation parent release case="<<seconds<<"s: all segments invalidated together after window removal\n";
    }
    {
        Config quota;quota.root=testTempRoot()+"/podcast-bound-cap";
        quota.diskBytes=quota.estimatedDiskBytes=32ull*8000*6;
        CoreQueueSink capped("Capped",quota,60);capped.useSpeakerClock();
        capped.estimate(1,20,0);capped.coreEvent("strm",1,0);
        capped.trackDescriptor({1,8000,{},0,0});capped.trackBoundary(0,{8000,24,2,false},false);
        std::vector<yeney::Frame> pcm(35*8000,{256,-256});
        assert(capped.write(pcm.data(),pcm.size(),false)==pcm.size());capped.waitIdle();
        capped.speakerPosition(1,15*8000,yeney::PlaybackState::Playing);capped.waitIdle();
        auto records=capped.snapshot();assert(records.size()==2 && records.back().promised==32*8000);
        assert(records.back().item->descriptor().kind==PromiseKind::UpperBound);
        capped.resolvedItemBoundary({1,0,pcm.size(),0,"no-transition"});capped.waitIdle();
        assert(capped.snapshot().back().item->producedFrames()==10*8000);
        std::cout<<"PASS continuation quota cap: 10 s overflow + 25 s parent capped at explicit 32 s PCM budget; clean 10 s early EOF\n";
    }
    for(unsigned seconds:{22u,45u,55u,80u}) {
        Config cfg;cfg.root=testTempRoot()+"/podcast-before-overflow";
        CoreQueueSink slow("Slow"+std::to_string(seconds),cfg,120);slow.useSpeakerClock();
        slow.estimate(1,20,0);slow.coreEvent("strm",1,0);
        slow.trackDescriptor({1,8000,{},0,0});slow.trackBoundary(0,{8000,24,2,false},false);
        std::vector<yeney::Frame> pcm(seconds*8000);
        for(size_t i=0;i<pcm.size();++i)pcm[i]={int32_t(i<<8),-int32_t(i<<8)};
        assert(slow.write(pcm.data(),20*8000,false)==20*8000);slow.waitIdle();
        slow.speakerPosition(1,14*8000,yeney::PlaybackState::Playing);slow.waitIdle();
        assert(slow.snapshot().size()==1);
        slow.speakerPosition(1,15*8000,yeney::PlaybackState::Playing);slow.waitIdle();
        auto records=slow.snapshot();assert(records.size()==2);
        assert(records.back().continuationDecision=="deadline-forced" && records.back().promised==25*8000);
        assert(records.back().item->producedFrames()==0);
        assert(slow.write(pcm.data()+20*8000,pcm.size()-20*8000,false)==pcm.size()-20*8000);
        slow.resolvedItemBoundary({1,0,pcm.size(),0,"no-transition"});slow.waitIdle();
        records=slow.snapshot();assert(records.size()==(seconds>50?3u:2u));
        std::vector<char> bytes;
        for(const auto &record:records) {
            auto reader=record.item->reader(44);char chunk[8192];
            while(auto n=reader.read(chunk,sizeof chunk))bytes.insert(bytes.end(),chunk,chunk+n);
        }
        assert(bytes.size()==pcm.size()*6);
        for(size_t i=0;i<pcm.size();++i)for(unsigned b=0;b<3;++b) {
            assert(uint8_t(bytes[i*6+b])==uint8_t(uint32_t(pcm[i].left)>>(8+b*8)));
            assert(uint8_t(bytes[i*6+3+b])==uint8_t(uint32_t(pcm[i].right)>>(8+b*8)));
        }
        yeney::AudibleStart start;assert(slow.takeAudibleStart(start) && start.occurrence==1);
        if(seconds==22) {
            slow.speakerPosition(1,22*8000,yeney::PlaybackState::Playing);
            slow.speakerPosition(records.back().source.occurrence,0,yeney::PlaybackState::Stopped);
            assert(slow.audibleElapsedMs(1)==22000 && slow.audibleFrames()==pcm.size());
            yeney::AudibleState state;bool stopped=false;
            while(slow.takeAudibleState(state))stopped=state.occurrence==1 && state.state==yeney::PlaybackState::Stopped;
            assert(stopped);
        }
        assert(!slow.takeAudibleStart(start));
        std::cout<<"PASS continuation pre-overflow deadline case="<<seconds
                 <<"s: published at 10 s lead with zero overflow; "
                 <<(seconds==22?"empty fallback ends with actual 22 s parent clock":seconds<=50?"one generous upper bound, clean early EOF":"further exact item only after bound exceeded")
                 <<"; sample-exact PCM, one LMS track\n";
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
    assert(records.size()==1 && records[0].promised==25*8000);
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
    assert(records.size()==2);
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
    assert(sink.audibleElapsedMs(1)==27000);
    sink.speakerPosition(records[1].source.occurrence,8000,yeney::PlaybackState::Playing);
    assert(sink.audibleElapsedMs(1)==26000);
    sink.restartCached();
    sink.speakerPosition(records[1].source.occurrence,8000,yeney::PlaybackState::Playing);
    assert(sink.takeAudibleStart(start) && start.occurrence==1 && start.frame==records[0].first);
    assert(sink.audibleElapsedMs(1)==26000);
    assert(sink.queueItemProduced(1));
    assert(sink.committedItemFrames(1)==pcm.size());
    std::cout<<"PASS podcast: resumed declaration ceil(20*8000)+5*8000, stale/resume clock never subtracted; sample-exact 46 s with an exact completed continuation; one LMS start and continuous seek clock\n";
    {
        Config quota=c;quota.diskBytes=quota.estimatedDiskBytes=4096;
        Store limited("EncodedQuota",quota);
        auto reservation=limited.reserveEncoded(true);
        assert(reservation && reservation->bytes==2048 && limited.diskBytes()==0);
        reservation->allocate(1024);assert(limited.diskBytes()==1024);
        bool exhausted=false;
        try { reservation->allocate(4097); } catch(const std::runtime_error &) { exhausted=true; }
        assert(exhausted && limited.diskBytes()==1024);
        limited.reserveEncoded(false);assert(limited.diskBytes()==0);
        std::cout<<"PASS encoded/PCM shared room quota: actual encoded file bytes counted, over-budget growth rejected atomically, release returns reservation\n";
    }
    {
        CoreQueueSink shortTail("TinyContinuation",c,60);shortTail.useSpeakerClock();
        shortTail.estimate(1,1,0);shortTail.coreEvent("strm",1,0);
        shortTail.trackDescriptor({1,8000,{},0,0});shortTail.trackBoundary(0,{8000,24,2,false},false);
        std::vector<yeney::Frame> audio(6*8000+8,{256,-256});
        assert(shortTail.write(audio.data(),audio.size(),false)==audio.size());
        shortTail.resolvedItemBoundary({1,0,audio.size(),0,"no-transition"});shortTail.waitIdle();
        auto segments=shortTail.snapshot();assert(segments.size()==2);
        shortTail.speakerPosition(segments.back().source.occurrence,8,yeney::PlaybackState::Stopped);
        yeney::AudibleStart signal;assert(!shortTail.takeAudibleStart(signal));
        shortTail.speakerPosition(1,5*8000,yeney::PlaybackState::Playing);
        assert(shortTail.takeAudibleStart(signal));
        shortTail.speakerPosition(segments.back().source.occurrence,8,yeney::PlaybackState::Stopped);
        assert(!shortTail.takeAudibleStart(signal) && shortTail.audibleFrames()==audio.size());
        yeney::AudibleState state;bool stopped=false;
        while(shortTail.takeAudibleState(state))stopped=state.occurrence==1 && state.state==yeney::PlaybackState::Stopped;
        assert(stopped);
        std::cout<<"PASS sub-poll continuation EOF: stopped final owned segment advances the established parent only, all frames counted, no second LMS start\n";
    }
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
