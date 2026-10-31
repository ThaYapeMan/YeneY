// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include "queue_declaration.h"
#include "queue_window.h"
#include "test_temp.h"
#include "upnp/queue_metadata.h"
#include <cassert>
#include <iostream>
#include <unistd.h>
using namespace queue_audio;

static void sameDeclaration(Store &store, const QueueRecord &natural, uint64_t expected, unsigned ordinal) {
    assert(natural.item && natural.item->contentLength() == 44 + expected * 6);
    auto lazy = store.create({"lazy-" + std::to_string(ordinal), natural.source.rate, expected, 0});
    assert(lazy->contentLength() == natural.item->contentLength());
    char declaredHeader[44], naturalHeader[44];
    assert(lazy->reader().read(declaredHeader, sizeof declaredHeader) == sizeof declaredHeader);
    assert(natural.item->reader().read(naturalHeader, sizeof naturalHeader) == sizeof naturalHeader);
    assert(std::equal(std::begin(declaredHeader), std::end(declaredHeader), std::begin(naturalHeader)));
    upnp::QueueMetadata declared, produced;
    declared.url = produced.url = "http://127.0.0.1/item";
    declared.title = produced.title = "Declaration fixture";
    declared.session = produced.session = "fixture";
    declared.rate = natural.source.rate; produced.rate = natural.item->descriptor().rate;
    declared.size = *lazy->contentLength(); produced.size = *natural.item->contentLength();
    declared.frames = expected; produced.frames = (*natural.item->contentLength() - 44) / 6;
    assert(upnp::queueDidl(declared) == upnp::queueDidl(produced));
    std::cout << "PASS exact declaration ordinal=" << ordinal << " rate=" << declared.rate
              << " frames=" << expected << " WAV_header=identical Content-Length=identical DIDL=identical\n";
}
int main() {
    std::string pattern = testTempRoot() + "/yeney-declarations-XXXXXX";
    const auto directory = mkdtemp(pattern.data());
    assert(directory);
    Config config; config.root = directory;
    {
        auto lazyConfig = config; lazyConfig.root /= "lazy";
        Store declarations("Declaration", lazyConfig);
        unsigned ordinal = 0;
        for (auto sourceRate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
            const auto source = declaredSource(sourceRate, 1001, 48000);
            const auto expected = sourceRate <= 48000 ? 1001u : sourceRate <= 96000 ? 501u : 251u;
            assert(source.frames == expected);
            assert(source.rate == (sourceRate == 44100 || sourceRate == 88200 || sourceRate == 176400 ? 44100u : 48000u));
            CoreQueueSink sink("Declaration", config, 1);
            yeney::TrackDescriptor descriptor{1, source.rate, source.frames, 0, 0};
            sink.coreEvent("strm", 1, 0);
            sink.trackDescriptor(descriptor);
            sink.trackBoundary(0, {source.rate,24,2,false}, false);
            std::vector<yeney::Frame> pcm(expected, {256,-256});
            sink.write(pcm.data(), pcm.size(), false);
            sink.resolvedItemBoundary({1,0,expected,0,"end-of-input"});
            sink.waitIdle();
            assert(declaredLocalFrames(descriptor, descriptor, 0) == expected);
            sameDeclaration(declarations, sink.snapshot().front(), expected, ++ordinal);
        }
        CoreQueueSink sink("Declaration", config, 1);
        const yeney::TrackDescriptor outgoing{1,8000,1000,1,1,300}, incoming{2,8000,300,0,0};
        sink.coreEvent("strm",1,0); sink.trackDescriptor(outgoing);
        sink.trackBoundary(0,{8000,24,2,false},false);
        std::vector<yeney::Frame> pcm(1000,{256,-256});
        sink.write(pcm.data(),100,false); sink.waitIdle();
        sink.trackDescriptor(incoming); sink.waitIdle();
        sink.write(pcm.data()+100,900,false);
        sink.resolvedItemBoundary({1,2,1000,0,"insufficient-incoming-pcm"});
        sink.trackBoundary(1000,{8000,24,2,false},true);
        sink.write(pcm.data(),300,false);
        sink.resolvedItemBoundary({2,0,1300,0,"end-of-input"}); sink.waitIdle();
        const auto records=sink.snapshot();
        assert(declaredLocalFrames(outgoing,outgoing,0)==700);
        assert(records[1].prefixFrames==300);
        assert(declaredLocalFrames(incoming,incoming,300)==600);
        sameDeclaration(declarations,records[0],700,++ordinal);
        sameDeclaration(declarations,records[1],600,++ordinal);
        auto unknown=incoming; unknown.sourceFrames={};
        assert(!declaredLocalFrames(unknown,incoming,0));
        assert(!declaredLocalFrames(outgoing, yeney::TrackDescriptor{3,8000,{},1,1,300},0));
        // Rate changes reject overlap, and a short source retains one frame.
        assert(declaredLocalFrames(outgoing, yeney::TrackDescriptor{3,48000,1000,1,1,300},0)==1000);
        assert(declaredLocalFrames(yeney::TrackDescriptor{3,8000,100,1,1,300},outgoing,0)==1);
        assert(declaredRemoteFrames(10.0001,44100)==661505);
        bool invalid=false;
        try { declaredRemoteFrames(std::numeric_limits<double>::infinity(),44100); }
        catch(const std::invalid_argument &) { invalid=true; }
        assert(invalid);
        std::cout << "PASS declarations: source-count rounding before publication, six source rates, outgoing overlap, carried prefix, unknown-count barrier, rate-change rejection, remote upper bound\n";
    }
    std::filesystem::remove_all(directory);
}
