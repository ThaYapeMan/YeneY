#include "test_temp.h"
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "pcm_pack.h"
#include "queue_item_store.h"
#include "upnp/stream_server.h"
#include "wav_writer.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace queue_audio;
using namespace std::chrono_literals;
namespace {
struct Temp {
    std::filesystem::path root;
    Temp() {
        std::string pattern = testTempRoot()+"/yeney-item-test-XXXXXX";
        auto p = mkdtemp(pattern.data());
        assert(p);
        root = p;
    }
    ~Temp() { std::filesystem::remove_all(root); }
};
std::vector<yeney::Frame> signal(size_t n, unsigned bits = 24) {
    PcmDither rng(1234);
    std::vector<yeney::Frame> data;
    auto sample = [&] {
        uint32_t v = rng.next();
        v &= bits == 16 ? 0xffff0000u : 0xffffff00u;
        return int32_t(v);
    };
    for (size_t i = 0; i < n; ++i)
        data.push_back({sample(), sample()});
    return data;
}
std::vector<char> reference(const std::vector<yeney::Frame> &frames, uint32_t rate) {
    auto h = wavHeader(rate, frames.size());
    std::vector<char> bytes(h.begin(), h.end());
    for (auto f : frames)
        for (auto v : {f.left, f.right})
            for (unsigned b = 1; b < 4; ++b)
                bytes.push_back(char(uint32_t(v) >> (8 * b)));
    return bytes;
}
std::vector<char> readAll(Reader r) {
    std::vector<char> bytes;
    char buffer[1301];
    for (;;) {
        auto n = r.read(buffer, sizeof(buffer));
        if (!n)
            return bytes;
        bytes.insert(bytes.end(), buffer, buffer + n);
    }
}
template <class F> void fails(F f, const std::string &message) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &e) {
        failed = true;
        assert(std::string(e.what()).find(message) != std::string::npos);
    }
    assert(failed);
}
int32_t unpack(const char *p, unsigned bytes) {
    uint32_t n = 0;
    for (unsigned i = 0; i < bytes; ++i)
        n |= uint32_t(uint8_t(p[i])) << (i * 8);
    return int32_t(n & (1u << (bytes * 8 - 1)) ? int64_t(n) - (int64_t(1) << (bytes * 8)) : n);
}
void packing() {
    PcmDither rng(19);
    char p[3];
    for (int v = -32768; v <= 32767; ++v) {
        packSonosSample(p, int32_t(int64_t(v) * 65536), 3, false, rng);
        assert(unpack(p, 3) == v * 256);
        packSonosSample(p, int32_t(int64_t(v) * 65536), 2, false, rng);
        assert(unpack(p, 2) == v);
    }
    for (unsigned i = 0; i < 200000; ++i) {
        auto v = int32_t(rng.next() & 0xffffff00u);
        packSonosSample(p, v, 3, false, rng);
        assert(int64_t(unpack(p, 3)) * 256 == v);
        v = int32_t(rng.next());
        packSonosSample(p, v, 3, true, rng);
        if (v < INT32_MAX - 128)
            assert(std::abs(double(unpack(p, 3)) - double(v) / 256) <= .5);
    }
    for (unsigned bytes : {2u, 3u})
        for (int32_t v : {INT32_MIN, INT32_MAX}) {
            packSonosSample(p, v, bytes, true, rng);
            assert(unpack(p, bytes) == (v < 0 ? -(1 << (bytes * 8 - 1)) : (1 << (bytes * 8 - 1)) - 1));
        }
    // Direct TPDF statistics and actual quantized silence use the same fixed
    // seed independently, checking distribution rather than reproducing RNG.
    PcmDither a(77), b(77), c(77), repeat(77);
    double sum = 0, squares = 0, output = 0;
    int low = 0, high = 0, pos = 0, neg = 0;
    const int count = 400000;
    for (int i = 0; i < count; ++i) {
        int x = a.tpdf16();
        assert(x == repeat.tpdf16());
        low = std::min(low, x);
        high = std::max(high, x);
        sum += double(x) / 65536;
        squares += std::pow(double(x) / 65536, 2);
        packSonosSample(p, 0, 2, true, b);
        int y = unpack(p, 2);
        assert(y >= -1 && y <= 1);
        output += y;
        pos += y > 0;
        neg += y < 0;
        packSonosSample(p, 0, 3, true, c);
        assert(unpack(p, 3) == 0);
    }
    assert(low > -65536 && low < -64000 && high < 65536 && high > 64000);
    assert(std::abs(sum / count) < .003 && std::abs(output / count) < .003);
    assert(std::abs(squares / count - 1.0 / 6) < .003);
    assert(pos > count * .12 && pos < count * .13 && neg > count * .12 && neg < count * .13);
    std::cout << "PASS packing: exact 16/24 bits, nearest24 error <=0.5 LSB, clipping; TPDF mean="
              << sum / count << " range=[" << low / 65536.0 << "," << high / 65536.0
              << "] variance=" << squares / count << "\n";
}
void history() {
    for (unsigned bits : {16u, 24u})
        for (uint32_t rate : {44100u, 48000u}) {
            Temp tmp;
            Config config;
            config.root = tmp.root;
            config.ramBytes = 24576;
            Store store("Study", config);
            auto pcm = signal(11000, bits);
            auto expected = reference(pcm, rate);
            auto item = store.create({"track", rate, pcm.size(), 3});
            store.protect(item, nullptr);
            assert(item->descriptor().generation == 3 && item->contentLength() == expected.size());
            item->append(pcm.data(), 1000, false);
            auto ram = item->reader(45);
            char shortRead[101];
            assert(ram.read(shortRead, 101) == 101);
            assert(std::equal(shortRead, shortRead + 101, expected.data() + 45));
            item->append(pcm.data() + 1000, pcm.size() - 1000, false);
            assert(store.ramBytes() <= config.ramBytes && store.diskBytes() > 0);
            assert(readAll(item->reader()) == expected); // mixed FLAC+RAM, not sealed yet
            item->finish();
            assert(store.ramBytes() == 0);
            auto first = std::async(std::launch::async, [&] { return readAll(item->reader()); });
            auto second = std::async(std::launch::async, [&] { return readAll(item->reader(103)); });
            assert(first.get() == expected);
            auto suffix = second.get();
            assert(std::equal(suffix.begin(), suffix.end(), expected.begin() + 103));
            for (size_t offset : {0u, 43u, 44u, 45u, 24575u, 24577u, 66043u}) {
                auto r = item->reader(offset);
                char bytes[29];
                auto n = r.read(bytes, sizeof(bytes));
                assert(std::equal(bytes, bytes + n, expected.data() + offset));
                assert(r.position() == offset + n);
            }
            for (const auto &f : std::filesystem::directory_iterator(tmp.root / "Study"))
                assert(f.path().filename() == ".lock" || f.path().extension() == ".flac");
        }
    std::cout << "PASS history: byte-identical RAM/FLAC WAV24 at 44.1/48 kHz from 16/24 bits, unaligned and "
                 "independent ranges\n";
}
void processedHistory() {
    Temp tmp;
    Config config;
    config.root = tmp.root;
    config.ramBytes = 6; // force even tiny processed blocks through FLAC
    Store store("Study", config);
    std::vector<yeney::Frame> pcm{{128, -128}, {384, -384}, {INT32_MAX, INT32_MIN}, {129, -129}};
    auto item = store.create({"processed", 48000, pcm.size(), 1});
    item->append(pcm.data(), pcm.size(), true);
    item->finish();
    auto actual = readAll(item->reader());
    auto header = wavHeader(48000, pcm.size());
    std::vector<char> expected(header.begin(), header.end());
    for (auto frame : pcm)
        for (auto sample : {frame.left, frame.right}) {
            const auto rounded = std::clamp<int64_t>(std::llround(double(sample) / 256), -8388608, 8388607);
            for (unsigned b = 0; b < 3; ++b)
                expected.push_back(char(uint32_t(rounded) >> (8 * b)));
        }
    assert(actual == expected && store.ramBytes() == 0);
    std::cout
        << "PASS processed history: nearest24 half ties and clipping survive one-frame FLAC spills exactly\n";
}
void waitsAndFaults() {
    Temp tmp;
    Config c;
    c.root = tmp.root;
    Store store("Study", c);
    auto pcm = signal(10);
    auto expected = reference(pcm, 44100);
    auto item = store.create({"future", 44100, 10, 1});
    std::vector<std::future<std::vector<char>>> waits;
    for (unsigned i = 0; i < 3; ++i)
        waits.push_back(std::async(std::launch::async, [&, i] { return readAll(item->reader(45 + i)); }));
    for (auto &f : waits)
        assert(f.wait_for(20ms) == std::future_status::timeout);
    item->append(pcm.data(), 10, false);
    item->finish();
    for (unsigned i = 0; i < 3; ++i) {
        auto bytes = waits[i].get();
        assert(std::equal(bytes.begin(), bytes.end(), expected.begin() + 45 + i));
    }
    auto cancel = store.create({"cancel", 44100, 10, 1});
    cancel->append(pcm.data(), 2, false);
    assert(store.ramBytes() > 0);
    waits.clear();
    for (int i = 0; i < 3; ++i)
        waits.push_back(std::async(std::launch::async, [&] { return readAll(cancel->reader(44)); }));
    for (auto &f : waits)
        assert(f.wait_for(20ms) == std::future_status::timeout);
    cancel->cancel("owner cancelled");
    assert(store.ramBytes() == 0);
    for (auto &f : waits)
        fails([&] { f.get(); }, "owner cancelled");
    auto shortItem = store.create({"short", 44100, 10, 1});
    shortItem->append(pcm.data(), 8, false);
    auto waiting = std::async(std::launch::async, [&] { return readAll(shortItem->reader(44 + 8 * 6)); });
    assert(waiting.wait_for(20ms) == std::future_status::timeout);
    try {
        shortItem->finish();
        assert(false);
    } catch (const LengthFault &f) {
        assert(f.expected == 10 && f.actual == 8);
    }
    fails([&] { waiting.get(); }, "expected=10 actual=8");
    auto longItem = store.create({"long", 44100, 8, 1});
    try {
        longItem->append(pcm.data(), 10, false);
        assert(false);
    } catch (const LengthFault &f) {
        assert(f.expected == 8 && f.actual == 10);
    }
    assert(longItem->producedFrames() == 0);
    fails([&] { readAll(longItem->reader(44 + 8 * 6)); }, "expected=8 actual=10");
    auto sealed = store.create({"sealed", 48000, std::nullopt, 1});
    assert(!sealed->contentLength());
    sealed->append(pcm.data(), 10, false);
    assert(!sealed->contentLength());
    auto sealWait = std::async(std::launch::async, [&] { return readAll(sealed->reader()); });
    assert(sealWait.wait_for(20ms) == std::future_status::timeout);
    sealed->finish();
    assert(sealWait.get() == reference(pcm, 48000));
    auto timeout = store.create({"timeout", 44100, 10, 1});
    char byte;
    fails([&] { timeout->reader(44).read(&byte, 1, 1ms); }, "timeout");
    auto empty = store.create({"empty", 44100, 0, 1});
    empty->finish();
    assert(readAll(empty->reader()).size() == 44);
    std::cout << "PASS promises: future ranges, unpublished sealed assets, exact/short/long/zero lengths, "
                 "timeout, cancellation wakes every reader\n";
}
std::vector<char> fileBytes(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void quotaAndStartup() {
    Temp tmp;
    Config c;
    c.root = tmp.root;
    c.ramBytes = 24576;
    std::filesystem::create_directories(tmp.root / "Study");
    std::ofstream(tmp.root / "Study" / "stale.wav") << "stale";
    auto pcm = signal(4096);
    uint64_t cost;
    {
        Store store("Study", c);
        assert(!std::filesystem::exists(tmp.root / "Study" / "stale.wav"));
        std::ofstream(tmp.root / "Study" / "keep-during-live-lock") << "guard";
        fails([&] { Store other("Study", c); }, "already open");
        assert(std::filesystem::exists(tmp.root / "Study" / "keep-during-live-lock"));
        auto a = store.create({"one", 44100, 4096, 1});
        a->append(pcm.data(), pcm.size(), false);
        a->finish();
        cost = store.diskBytes();
        assert(cost > 24000);
    }
    c.diskBytes = cost * 2 + cost / 2;
    Store store("Study", c);
    assert(!std::filesystem::exists(tmp.root / "Study" / "keep-during-live-lock"));
    auto make = [&](std::string name) {
        auto i = store.create({name, 44100, 4096, 1});
        i->append(pcm.data(), pcm.size(), false);
        i->finish();
        return i;
    };
    auto a = make("a"), b = make("b");
    assert(fileBytes(tmp.root / "Study" / "1-0.flac") == fileBytes(tmp.root / "Study" / "2-0.flac"));
    auto d = store.create({"c", 44100, 4096, 1});
    store.protect(d, nullptr);
    d->append(pcm.data(), pcm.size(), false);
    d->finish();
    fails([&] { readAll(a->reader()); }, "evicted");
    assert(readAll(b->reader()) == reference(pcm, 44100));
    store.protect(b, d);
    auto e = store.create({"d", 44100, 4096, 1});
    e->append(pcm.data(), pcm.size(), false);
    fails([&] { e->finish(); }, "quota exhausted");
    assert(readAll(b->reader()) == reference(pcm, 44100) && readAll(d->reader()) == reference(pcm, 44100));
    uint64_t actual = 0;
    for (auto &f : std::filesystem::directory_iterator(tmp.root / "Study"))
        if (f.path().extension() == ".flac")
            actual += f.file_size();
    assert(store.diskBytes() == actual && actual <= c.diskBytes);
    auto sameToken = store.create({"a", 44100, 0, 2});
    assert(sameToken->descriptor().generation == 2);
    fails([&] { store.create({"a", 44100, 0, 2}); }, "duplicate");
    fails([&] { Store invalid("../outside", c); }, "invalid");
    fails([&] { store.create({"bad", 44100, 1, 1, 1, 24}); }, "invalid");
    fails([&] { wavLength(UINT64_MAX); }, "RIFF");
    std::cout << "PASS quota/startup: deterministic FLAC, oldest eviction, current/next protected, bounded "
                 "disk/RAM, exclusive room cleanup, immutable generations\n";
}
void lifecycleAndCorruption() {
    Temp tmp;
    Config c;
    c.root = tmp.root;
    std::shared_ptr<Item> retained;
    std::future<std::vector<char>> waiting;
    {
        Store store("Study", c);
        retained = store.create({"lifetime", 44100, 10, 1});
        waiting = std::async(std::launch::async, [&] { return readAll(retained->reader(44)); });
        assert(waiting.wait_for(20ms) == std::future_status::timeout);
    }
    fails([&] { waiting.get(); }, "store closed");
    Store reopened("Study", c);
    auto pcm = signal(4096);
    auto corrupt = reopened.create({"corrupt", 44100, 4096, 1});
    corrupt->append(pcm.data(), pcm.size(), false);
    corrupt->finish();
    {
        std::fstream file(tmp.root / "Study" / "1-0.flac", std::ios::binary | std::ios::in | std::ios::out);
        file << "BAD!";
    }
    fails([&] { readAll(corrupt->reader(44)); }, "FLAC");
    fails([&] { readAll(corrupt->reader(44)); }, "FLAC");
    std::cout << "PASS lifecycle: store shutdown wakes readers, reopen with old handles, corrupt history "
                 "fails all readers\n";
}
struct Request : upnp::StreamRequest {
    Method verb = Method::Get;
    std::string range, wire;
    bool disconnected = false;
    std::string path() const override { return "/test.wav"; }
    Method method() const override { return verb; }
    std::string parameter(const std::string &) const override { return {}; }
    upnp::RequestHeaders headers() const override { return {{"Range", range}}; }
    bool send(const char *p, size_t n) override {
        wire.append(p, n);
        return true;
    }
    bool peerClosed() override { return false; }
    void sendTimeout(unsigned) override {}
    void disconnect() override { disconnected = true; }
    void reply(unsigned status, const std::string & = {}) override { wire = std::to_string(status); }
    std::string serverName() const override { return "test"; }
};
void http() {
    Temp tmp;
    Config c;
    c.root = tmp.root;
    Store store("Study", c);
    auto pcm = signal(40);
    auto expected = reference(pcm, 44100);
    auto item = store.create({"http", 44100, 40, 1});
    Request q;
    q.range = "bytes=49-100";
    auto response = std::async(std::launch::async, [&] { serveWav(q, item, 1s); });
    assert(response.wait_for(20ms) == std::future_status::timeout);
    item->append(pcm.data(), 40, false);
    item->finish();
    response.get();
    assert(q.wire.find("206 Partial Content") != std::string::npos);
    assert(q.wire.find("Content-Length: 52\r\n") != std::string::npos);
    auto body = q.wire.substr(q.wire.find("\r\n\r\n") + 4);
    assert(std::equal(body.begin(), body.end(), expected.begin() + 49));
    Request whole;
    serveWav(whole, item);
    assert(whole.wire.substr(whole.wire.find("\r\n\r\n") + 4) ==
           std::string(expected.begin(), expected.end()));
    Request head;
    head.verb = Request::Method::Head;
    head.range = "bytes=-7";
    serveWav(head, item);
    assert(head.wire.find("Content-Length: 7\r\n") != std::string::npos &&
           head.wire.size() == head.wire.find("\r\n\r\n") + 4);
    Request invalid;
    invalid.range = "bytes=999-";
    serveWav(invalid, item);
    assert(invalid.wire.find("416 ") != std::string::npos);
    for (auto bad :
         {"bytes=0-1,3-4", "bytes=-0", "bytes=-", "bytes=+2-3", "bytes=10-2", "bytes=18446744073709551616-"})
        fails([&] { wavRange(bad, 100); }, "");
    auto pending = store.create({"cancelhttp", 44100, 40, 1});
    Request cancelled;
    cancelled.range = "bytes=44-";
    auto cancelWait = std::async(std::launch::async, [&] { serveWav(cancelled, pending, 1s); });
    assert(cancelWait.wait_for(20ms) == std::future_status::timeout);
    pending->cancel();
    cancelWait.get();
    assert(cancelled.disconnected);
    auto unknown = store.create({"unknown", 44100, std::nullopt, 1});
    Request unavailable;
    serveWav(unavailable, unknown);
    assert(unavailable.wire == "503");
    setenv("YENEY_QUEUE_DISK_MIB", "17", 1);
    assert(Config::environment().diskBytes == 17ull * 1024 * 1024);
    for (auto bad : {"0", "-1", "abc", "18446744073709551615"}) {
        setenv("YENEY_QUEUE_DISK_MIB", bad, 1);
        fails([] { Config::environment(); }, "");
    }
    unsetenv("YENEY_QUEUE_DISK_MIB");
    std::cout << "PASS HTTP helper: progressive unaligned 206, full 200, HEAD/suffix, 416, "
                 "cancellation/disconnect, sealed admission and quota environment\n";
}
} // namespace
int main() {
    packing();
    history();
    processedHistory();
    waitsAndFaults();
    quotaAndStartup();
    lifecycleAndCorruption();
    http();
}
