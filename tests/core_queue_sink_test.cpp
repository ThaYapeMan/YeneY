// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include "queue_report.h"
#include <cassert>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace queue_audio;
int main() {
    char pattern[] = "/tmp/yeney-queue-sink-XXXXXX";
    auto root = mkdtemp(pattern);
    assert(root);
    Config c;
    c.root = root;
    c.ramBytes = 6 * 64;
    {
        CoreQueueSink sink("Test", c, 1);
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 1000, 0, 0});
        sink.trackBoundary(0, {8000, 16, 2, false}, false);
        std::vector<yeney::Frame> pcm(1000, {123 * 65536, -456 * 65536});
        assert(sink.write(pcm.data(), 400, false) == 400);
        sink.waitIdle();
        sink.coreEvent("decode-complete", 1, 1000);
        sink.coreEvent("STMd", 1, 1000);
        sink.coreEvent("strm", 2, 0);
        sink.trackDescriptor({2, 48000, 1000, 0, 0});
        sink.waitIdle();
        auto rows = sink.snapshot();
        assert(rows[0].publication == "progressive" && rows[0].promised == 1000);
        auto reader = rows[0].item->reader(44 + 900 * 6);
        auto waiting = std::async(std::launch::async, [reader]() mutable {
            char b[6];
            return reader.read(b, 6);
        });
        assert(sink.write(pcm.data() + 400, 600, false) == 600);
        sink.resolvedItemBoundary({1, 2, 1000, 0, "different-rates"});
        sink.trackBoundary(1000, {48000, 24, 2, false}, true);
        assert(sink.write(pcm.data(), 1000, false) == 1000);
        sink.resolvedItemBoundary({2, 0, 2000, 0, "end-of-input"});
        sink.waitIdle();
        assert(waiting.get() == 6);
        rows = sink.snapshot();
        assert(rows[1].publication == "progressive");
        yeney::AudibleStart a;
        assert(!sink.takeAudibleStart(a));
        sink.audibleFrom(1, 0);
        assert(!sink.takeAudibleStart(a));
        sink.advanceAudible(1);
        assert(sink.takeAudibleStart(a) && a.occurrence == 1);
        sink.advanceAudible(1000);
        sink.audibleFrom(2, 1000);
        sink.advanceAudible(1001);
        assert(sink.takeAudibleStart(a) && a.occurrence == 2);
        std::cout << "PASS queue sink: worker store, progressive commitment, "
                     "immediate exact commitment, independent future reader and "
                     "explicit audible signals\n";
    }
    {
        CoreQueueSink sink("Tail", c, 1);
        std::vector<yeney::Frame> pcm(1000, {256, -256});
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 1000, 0, 0});
        sink.trackBoundary(0, {8000, 24, 2, false}, false);
        sink.write(pcm.data(), 100, false);
        sink.waitIdle();
        // Real successor commits all 1000 frames to overlap: promised old count
        // zero.
        sink.trackDescriptor({2, 8000, 1000, 1, 1, 8000});
        sink.waitIdle();
        // Too late to promise zero after 100 produced: sealed, never trim.
        assert(sink.snapshot()[0].publication == "progressive");
        assert(sink.snapshot()[0].promised == 1000);
        sink.write(pcm.data(), 900, false);
        sink.resolvedItemBoundary({1, 2, 1000, 0, "insufficient-incoming-pcm"});
        sink.waitIdle();
        std::cout << "PASS queue sink: disproved impossible commitment remains "
                     "immutable without lost PCM\n";
    }
    {
        CoreQueueSink sink("Continuation", c, 1);
        std::vector<yeney::Frame> pcm(1000, {256, -256});
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 1000, 1, 1, 300});
        sink.trackBoundary(0, {8000, 24, 2, false}, false);
        sink.write(pcm.data(), 100, false);
        sink.waitIdle();
        sink.trackDescriptor({2, 8000, 300, 0, 0});
        sink.waitIdle();
        assert(sink.snapshot()[0].promised == 700);
        // The incoming occurrence is replaced by a different-rate source after
        // commitment.
        sink.trackDescriptor({2, 48000, 300, 0, 0});
        sink.write(pcm.data() + 100, 900, false);
        sink.resolvedItemBoundary({1, 2, 1000, 0, "different-rates"});
        sink.trackBoundary(1000, {48000, 24, 2, false}, true);
        sink.write(pcm.data(), 300, false);
        sink.resolvedItemBoundary({2, 0, 1300, 0, "end-of-input"});
        sink.waitIdle();
        auto rows = sink.snapshot();
        assert(rows.size() == 3 && rows[0].item->producedFrames() == 700);
        assert(rows[2].item->descriptor().rate == 8000 && rows[2].item->producedFrames() == 300);
        assert(rows[1].item->descriptor().rate == 48000 && rows[1].item->producedFrames() == 300);
        assert(rows[0].actual + rows[1].actual + rows[2].actual == 1300);
        std::cout << "PASS queue promise recovery: immutable 700-frame item, "
                     "300-frame old-rate "
                     "continuation, different-rate successor; no PCM loss\n";
    }
    {
        CoreQueueSink sink("SameRateTail", c, 1);
        std::vector<yeney::Frame> pcm(1000, {256, -256});
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 1000, 1, 1, 300});
        sink.trackBoundary(0, {8000, 24, 2, false}, false);
        sink.write(pcm.data(), 100, false);
        sink.waitIdle();
        sink.trackDescriptor({2, 8000, 300, 0, 0});
        sink.waitIdle();
        sink.write(pcm.data() + 100, 900, false);
        sink.resolvedItemBoundary({1, 2, 1000, 0, "insufficient-incoming-pcm"});
        sink.trackBoundary(1000, {8000, 24, 2, false}, true);
        sink.write(pcm.data(), 300, false);
        sink.resolvedItemBoundary({2, 0, 1300, 0, "end-of-input"});
        sink.waitIdle();
        auto rows = sink.snapshot();
        assert(rows.size() == 2 && rows[1].prefixFrames == 300);
        assert(rows[0].actual == 700 && rows[1].actual == 600 && rows[1].first == 700);
        sink.audibleFrom(1, 0);
        sink.advanceAudible(1);
        assert(sink.audibleOccurrence() == 1);
        yeney::AudibleStart a;
        assert(sink.takeAudibleStart(a));
        sink.advanceAudible(700);
        sink.audibleFrom(2, 700);
        assert(sink.audibleOccurrence() == 1);
        assert(!sink.takeAudibleStart(a));
        sink.advanceAudible(999);
        assert(sink.audibleOccurrence() == 1);
        assert(!sink.takeAudibleStart(a));
        sink.advanceAudible(1001);
        assert(sink.audibleOccurrence() == 2);
        assert(sink.takeAudibleStart(a) && a.occurrence == 2 && a.frame == 1000);
        sink.useSpeakerClock();
        sink.speakerPosition(1, 0, yeney::PlaybackState::Playing);
        assert(sink.takeAudibleStart(a));
        sink.speakerPosition(1, 700, yeney::PlaybackState::Playing);
        sink.speakerPosition(2, 0, yeney::PlaybackState::Playing);
        assert(sink.audibleOccurrence() == 1 && !sink.takeAudibleStart(a));
        assert(sink.audibleElapsedMs(1) == 87);
        sink.speakerPosition(2, 299, yeney::PlaybackState::Paused);
        assert(sink.audibleOccurrence() == 1 && !sink.takeAudibleStart(a));
        assert(sink.audibleElapsedMs(1) == 124);
        sink.speakerPosition(2, 300, yeney::PlaybackState::Playing);
        assert(sink.takeAudibleStart(a) && a.frame == 1000 && a.confirmed);
        assert(sink.audibleElapsedMs(2) == 0 && !sink.audibleElapsedMs(1));
        std::cout << "PASS speaker retained prefix: old source clock advances through prefix, incoming "
                     "confirmed start and zero only at source boundary\n";
        std::cout
            << "PASS same-rate tail recovery: successor carries 300 retained "
               "frames; incoming STMs "
               "waits until the prefix ends; LMS checks retain the outgoing source during that prefix\n";
    }
    {
        CoreQueueSink sink("BlockedIO", c, 1);
        std::vector<yeney::Frame> pcm(512, {256, -256});
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 10000, 0, 0});
        sink.trackBoundary(0, {8000, 24, 2, false}, false);
        sink.write(pcm.data(), 100, false);
        sink.waitIdle();
        sink.trackDescriptor({2, 8000, 10000, 0, 0});
        sink.waitIdle();
        auto item = sink.snapshot()[0].item;
        const auto fifo = std::filesystem::path(root) / "BlockedIO" / "1-64.flac";
        assert(mkfifo(fifo.c_str(), 0600) == 0);
        // Force the worker's next spill to wait on disk without holding reader
        // metadata.
        assert(sink.write(pcm.data(), 512, false) == 512);
        auto reader = std::async(std::launch::async, [item] {
            char h[44];
            assert(item->reader(44).read(h, 6) == 6);
            return item->reader().read(h, 44);
        });
        assert(reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready && reader.get() == 44);
        size_t accepted = 512;
        for (unsigned i = 0; i < 100; ++i)
            accepted += sink.write(pcm.data(), 512, false);
        assert(accepted <= 8000); // bounded in seconds, including the in-flight worker batch
        auto drain = std::async(std::launch::async, [fifo] {
            int fd = open(fifo.c_str(), O_RDONLY);
            assert(fd >= 0);
            char b[4096];
            while (read(fd, b, sizeof(b)) > 0) {
            }
            close(fd);
        });
        item->cancel("blocked I/O cancelled");
        drain.get();
        try {
            sink.waitIdle();
        } catch (const std::exception &) {
        }
        std::cout << "PASS queue worker backpressure: producer admission bounded to 1 s; "
                     "reader proceeds "
                     "while history disk write is blocked; cancellation completes\n";
    }
    {
        std::vector<QueueRecord> records(2);
        records[0].source = {1, 44100, 1000, 0, 0};
        records[0].end = 20;
        records[0].lms.push_back({1, "0", "1", "Track", true});
        records[1].source = {2, 48000, std::nullopt, 0, 0};
        records[1].strm = 8;
        std::ostringstream md, tsv;
        auto v = writeReadAheadReport(records, md, tsv);
        assert(v.ok && v.worst == 12 && v.pairs == 1);
        assert(md.str().find("LMS read-ahead OK") != std::string::npos);
        assert(tsv.str().find("promised_frames\tactual_frames") != std::string::npos);
        records[1].strm = 8.001;
        md.str("");
        tsv.str("");
        assert(!writeReadAheadReport(records, md, tsv).ok);
        records[0].end = -1;
        assert(!writeReadAheadReport(records, md, tsv).ok);
        records.clear();
        assert(!writeReadAheadReport(records, md, tsv).ok);
        std::cout << "PASS dry-run report: exact 12-second threshold, worst lead, "
                     "unobserved end and "
                     "empty-run failure; TSV and Markdown verdict\n";
    }
    {
        std::vector<QueueRecord> rows(2);
        rows[0].end = 30;
        rows[1].strm = 1;
        std::ostringstream md, tsv;
        auto v = writeReadAheadReport(rows, md, tsv);
        assert(!v.ok && v.reasons.find("lms-cli-unavailable") != std::string::npos);
        rows[0].lms.push_back({1, "0", "1", "Track", true});
        rows[0].lms.push_back({2, "", "", "", std::nullopt});
        md.str("");
        assert(writeReadAheadReport(rows, md, tsv).ok);
        assert(md.str().find("WARNING: LMS CLI unknown") != std::string::npos);
        std::cout << "PASS LMS CLI coverage: zero successful polls fail; >10% unknown warns\n";
    }
    {
        CoreQueueSink sink("EarlySuccessor", c, 1);
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 1000, 1, 1, 300});
        sink.coreEvent("strm", 2, 0);
        sink.trackDescriptor({2, 8000, 300, 0, 0});
        sink.trackBoundary(0, {8000, 24, 2, false}, false);
        sink.waitIdle();
        auto r = sink.snapshot();
        assert(r[0].publication == "progressive" && r[0].promised == 700);
        std::cout << "PASS early successor: progressive publication also works "
                     "when successor metadata "
                     "precedes the first output boundary\n";
    }
    {
        std::vector<QueueRecord> rows(5);
        for (size_t i = 0; i < rows.size(); ++i) {
            rows[i].source.occurrence = i + 1;
            rows[i].strm = i * 20;
        }
        rows[0].end = 40;
        rows[0].lms.push_back({1, "0", "1", "Track", true});
        rows[1].end = 60;
        rows[2].fault = "source error";
        rows[3].incomplete = "flushed";
        std::ostringstream md, tsv;
        auto v = writeReadAheadReport(rows, md, tsv);
        assert(v.ok && v.pairs == 2 && v.incomplete == 3 && v.worst == 20);
        assert(tsv.str().find("source-fault") != std::string::npos);
        assert(tsv.str().find("run-end") != std::string::npos);
        assert(tsv.str().find("flushed") != std::string::npos);
        rows[2].storageFault = true;
        assert(!writeReadAheadReport(rows, md, tsv).ok);
        rows[2].storageFault = false;
        rows[0].lmsMismatch = true;
        assert(!writeReadAheadReport(rows, md, tsv).ok);
        assert(md.str().find("lms-track-mismatch") != std::string::npos);
        std::cout << "PASS completed-pair verdict: unfinished, source-fault and flushed rows excluded; "
                     "storage and sustained LMS mismatch fail\n";
    }
    {
        CoreQueueSink sink("LmsSamples", c, 1);
        sink.coreEvent("strm", 1, 0);
        sink.waitIdle();
        sink.recordLms(1, {1, "1", "0", "wrong", false});
        sink.recordLms(1, {2, "1", "1", "wrong", false});
        sink.recordLms(1, {3, "1", "2", "wrong", false});
        assert(!sink.snapshot()[0].lmsMismatch);
        sink.recordLms(1, {4.01, "1", "3", "wrong", false});
        assert(sink.snapshot()[0].lmsMismatch && sink.snapshot()[0].mismatchedSeconds == 4);
        std::cout << "PASS LMS sampling: boundary tolerance through 2 seconds, sustained mismatch counted "
                     "and fails\n";
    }
    {
        CoreQueueSink sink("SourceLengthFault", c, 1);
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 1000, 0, 0});
        sink.trackBoundary(0, {8000, 24, 2, false}, false);
        std::vector<yeney::Frame> pcm(100, {256, -256});
        sink.write(pcm.data(), pcm.size(), false);
        sink.resolvedItemBoundary({1, 0, 100, 0, "no-transition"});
        bool fault = false;
        try {
            sink.waitIdle();
        } catch (const std::exception &) {
            fault = true;
        }
        auto row = sink.snapshot().front();
        assert(fault && !row.storageFault && row.fault.find("length fault") != std::string::npos);
        std::cout
            << "PASS source length fault: source-short beyond policy is not mislabeled as a storage fault\n";
    }
    {
        std::vector<QueueRecord> rows(2);
        rows[0].source.occurrence = 1;
        rows[0].end = 20;
        rows[0].lms.push_back({1, "0", "1", "Track", true});
        rows[1].source.occurrence = 2;
        rows[1].strm = 8;
        rows[1].prefixFrames = 100;
        std::ostringstream md, tsv;
        assert(writeReadAheadReport(rows, md, tsv).pairs == 0);
        rows[1].audible = 21;
        auto v = writeReadAheadReport(rows, md, tsv);
        assert(v.ok && v.pairs == 1 && v.worst == 13);
        rows[1].prefixFrames = 0;
        rows.emplace_back();
        rows[2].source.occurrence = UINT64_MAX - 1;
        assert(writeReadAheadReport(rows, md, tsv).pairs == 0);
        rows[2].end = 22;
        v = writeReadAheadReport(rows, md, tsv);
        assert(v.ok && v.pairs == 1 && v.worst == 14);
        std::cout << "PASS retained-tail report: pairs stay incomplete until prefix or continuation ends; "
                     "audible end includes all retained source PCM\n";
    }
    {
        CoreQueueSink sink("SpeakerZero", c, 1);
        sink.useSpeakerClock();
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 48000, 1000, 0, 0});
        sink.trackBoundary(0, {48000, 24, 2, false}, false);
        sink.waitIdle();
        sink.speakerPosition(1, 0, yeney::PlaybackState::Playing);
        yeney::AudibleStart start;
        assert(!sink.takeAudibleStart(start)); // no source frames yet
        std::vector<yeney::Frame> pcm(1000, {256, -256});
        sink.write(pcm.data(), pcm.size(), false);
        sink.waitIdle();
        sink.speakerPosition(1, 0, yeney::PlaybackState::Playing);
        assert(sink.takeAudibleStart(start) && start.occurrence == 1 && start.confirmed);
        assert(sink.audibleFrames() == 0 && sink.audibleElapsedMs(1) == 0);
        assert(!sink.audibleElapsedMs(2)); // elapsed belongs to this occurrence only
        sink.speakerPosition(1, 960, yeney::PlaybackState::Playing);
        assert(sink.audibleElapsedMs(1) == 20);
        sink.speakerPosition(1, 480, yeney::PlaybackState::Paused);
        assert(sink.audibleElapsedMs(1) == 20);
        sink.speakerPosition(1, 1000000, yeney::PlaybackState::Playing);
        assert(sink.audibleFrames() == 1000 && sink.audibleElapsedMs(1) == 20);
        assert(!sink.takeAudibleStart(start));
        sink.flush();
        sink.waitIdle();
        yeney::AudibleState state;
        assert(!sink.takeAudibleState(state) && !sink.audibleElapsedMs(1));
        std::cout << "PASS speaker sink: confirmed zero keeps internal zero, source frames required, no "
                     "duplicate starts, occurrence-local monotonic capped elapsed, flush fences state\n";
    }
    {
        CoreQueueSink sink("CancelledDescriptor", c, 1);
        sink.coreEvent("strm", 1, 0);
        sink.trackDescriptor({1, 8000, 100, 0, 0});
        sink.coreEvent("queue-refetch", 1, 0); // no PCM boundary existed
        sink.coreEvent("strm", 2, 0);
        sink.trackDescriptor({2, 8000, 100, 0, 0});
        sink.trackBoundary(0, {8000, 16, 2, false}, false);
        std::vector<yeney::Frame> pcm(100, {65536, -65536});
        assert(sink.write(pcm.data(), pcm.size(), false) == pcm.size());
        sink.resolvedItemBoundary({2, 0, 100, 0, "replacement"});
        sink.waitIdle();
        auto records = sink.snapshot();
        assert(records.size() == 2 && !records[0].item && !records[0].incomplete.empty());
        assert(records[1].item && records[1].item->descriptor().occurrence == "2");
        assert(records[1].item->producedFrames() == 100 && sink.latestOccurrence() == 2);
        sink.flush();
        sink.waitIdle();
        assert(sink.latestOccurrence() == 2); // only actual core starts allocate occurrences
        std::cout << "PASS cancelled queue descriptor: unrendered producer removed, replacement owns its "
                     "PCM/identity, reservations never advance the actual core occurrence\n";
    }
    std::filesystem::remove_all(root);
}
