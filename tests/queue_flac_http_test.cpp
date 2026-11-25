// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "flac_writer.h"
#include "test_temp.h"
#include "upnp/http_server.h"
#include <cassert>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

using namespace queue_audio;
using namespace std::chrono_literals;

int main() {
    setenv("YENEY_QUEUE_PACING", "off", 1); // Byte comparisons run faster than real time.
    auto temp = testTempRoot() + "/yeney-flac-http-XXXXXX";
    assert(mkdtemp(temp.data()));
    {
        Config config;
        config.packRows = true;
        config.root = temp;
        Store store("Study", config);
        Route route("Study", "session", Format::FlacPacked);
        constexpr size_t frames = 1537;
        std::vector<yeney::Frame> pcm(frames, {0x12345600, -0x12345600});
        auto item = store.create({"exact", 44100, frames, 1});
        auto url = route.publish("Study", 1, 1, item);
        auto get = [&](const std::string &path, const std::string &method = "GET", const std::string &range = "", Route *on = nullptr) {
            std::string wire;
            upnp::HttpRequestIO io;
            io.send = [&](const char *bytes, size_t count) { wire.append(bytes, count); return true; };
            io.peerClosed = io.aborted = [] { return false; };
            io.disconnect = [] {};
            io.sendTimeout = [](unsigned) {};
            auto request = upnp::httpRequestFromHeaders(method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" +
                (range.empty() ? "" : "Range: bytes=" + range + "\r\n") + "\r\n", io);
            if (on) {
                assert(request && on->serve(*request));
            } else {
                assert(request && route.serve(*request));
            }
            return wire;
        };
        const auto body = [](const std::string &wire) { return wire.substr(wire.find("\r\n\r\n") + 4); };
        const auto head = get(url, "HEAD");
        assert(url == "/yeney/q/session/1/1/exact.flac");
        assert(head.find("Content-Type: audio/flac\r\n") != std::string::npos);
        assert(body(head).empty());
        assert(get("/yeney/q/session/1/1/exact.wav").find("404") != std::string::npos);

        item->append(pcm.data(), 256, false);
        auto reading = std::async(std::launch::async, [&] { return get(url, "GET", "1600-"); });
        assert(reading.wait_for(100ms) == std::future_status::timeout);
        item->append(pcm.data() + 256, frames - 256, false);
        item->finish();
        assert(reading.get() == get(url, "GET", "1600-"));
        std::cout << "PASS FLAC HTTP waits for future frames and delivers the finished bytes\n";

        const auto whole = get(url);
        const auto bytes = body(whole);
        assert(whole.find("HTTP/1.1 200 OK\r\n") == 0);
        assert(bytes.compare(0, 4, "fLaC") == 0);
        assert(head.find("Content-Length: " + std::to_string(bytes.size()) + "\r\n") != std::string::npos);
        struct Range { std::string text; size_t first, last; };
        const auto last = bytes.size() - 1;
        const Range ranges[] = {
            {"0-0", 0, 0}, {"0-41", 0, 41}, {"41-42", 41, 42}, {"42-42", 42, 42},
            {"47-48", 47, 48}, {"1525-1608", 1525, 1608}, {"100-3000", 100, 3000},
            {"3000-3000", 3000, 3000}, {"4000-7400", 4000, 7400}, {"5000-5002", 5000, 5002},
            {"36-50", 36, 50}, {"1-500", 1, 500}, {"4500-", 4500, last}, {"0-", 0, last},
            {"-1", last, last}, {"-101", last - 100, last}, {"-999999", 0, last},
            {std::to_string(last) + "-", last, last}, {"9000-999999", 9000, last},
        };
        for (const auto &range : ranges) {
            const auto response = get(url, "GET", range.text);
            const auto count = range.last - range.first + 1;
            assert(response.find("HTTP/1.1 206 Partial Content\r\n") == 0);
            assert(response.find("Content-Length: " + std::to_string(count) + "\r\n") != std::string::npos);
            assert(response.find("Accept-Ranges: bytes\r\n") != std::string::npos);
            const auto info = "Content-Range: bytes " + std::to_string(range.first) + "-" +
                std::to_string(range.last) + "/" + std::to_string(bytes.size()) + "\r\n";
            assert(response.find(info) != std::string::npos);
            assert(body(response) == bytes.substr(range.first, count));
        }
        const auto invalid = get(url, "GET", std::to_string(last + 1) + "-");
        assert(invalid == "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
            std::to_string(bytes.size()) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        const auto layout = FlacLayout(frames, 44100);
        for (auto start : {layout.offset(1), layout.offset(1) + 31}) {
            const auto partial = get(url, "GET", std::to_string(start) + "-");
            assert(body(partial) == bytes.substr(start));
            assert(partial.find("Content-Length: " + std::to_string(bytes.size() - start) + "\r\n") != std::string::npos);
        }
        const auto otherRate = store.create({"other-rate", 32000, frames, 1});
        const auto otherUrl = route.publish("Study", 1, 1, otherRate);
        otherRate->append(pcm.data(), pcm.size(), false);
        otherRate->finish();
        std::cout << "32 kHz exact row: " << otherUrl << std::endl;
        assert(otherUrl == "/yeney/q/session/1/1/other-rate.wav");
        assert(get(otherUrl, "HEAD").find("Content-Type: audio/wav\r\n") != std::string::npos);
        assert(body(get(otherUrl)).compare(0, 4, "RIFF") == 0);
        const auto awaitPacked = [&](const std::shared_ptr<Item> &row) {
            for (auto tries = 0; tries < 300 && !row->packed(24); ++tries)
                std::this_thread::sleep_for(10ms);
            assert(row->packed(24));
            return row->packed(24);
        };
        awaitPacked(item);
        assert(body(get(url)) == bytes);
        assert(get(url, "HEAD").find("Content-Length: " + std::to_string(bytes.size()) + "\r\n") != std::string::npos);
        const auto prepared = store.create({"prepared", 44100, frames, 1});
        const auto readyUrl = route.publish("Study", 1, 1, prepared);
        prepared->append(pcm.data(), pcm.size(), false);
        prepared->finish();
        const auto packed = awaitPacked(prepared);
        assert(packed->size() != layout.size());
        assert(get(readyUrl, "HEAD").find("Content-Length: " + std::to_string(packed->size()) + "\r\n") != std::string::npos);
        const auto packedBody = body(get(readyUrl));
        std::string expected(packed->size(), '\0');
        assert(packed->read(0, expected.data(), expected.size()) == expected.size());
        assert(packedBody == expected);
        const auto middle = packed->offset(1) + 1;
        assert(body(get(readyUrl, "GET", std::to_string(middle) + "-")) == expected.substr(middle));
        std::cout << "PASS HEAD locks fixed and packed forms, and packed HTTP serves its own length and byte ranges\n";
        Route fixedRoute("Study", "fixed-setting", Format::Flac);
        const auto fixedUrl = fixedRoute.publish("Study", 1, 1, prepared);
        assert(body(get(fixedUrl, "GET", "", &fixedRoute)) == bytes);

        setenv("YENEY_QUEUE_PACING", "on", 1);
        setenv("YENEY_QUEUE_PACE_RATE", "2", 1);
        setenv("YENEY_QUEUE_PACE_FREE_MS", "2000", 1);
        setenv("YENEY_QUEUE_PACE_TAIL_MS", "0", 1);
        Route pacedRoute("Study", "paced-packed", Format::FlacPacked);
        const std::vector<yeney::Frame> silence(8 * 44100);
        const auto pacedItem = store.create({"paced", 44100, silence.size(), 1});
        const auto pacedUrl = pacedRoute.publish("Study", 1, 1, pacedItem);
        pacedItem->append(silence.data(), silence.size(), false);
        pacedItem->finish();
        const auto pacedCopy = awaitPacked(pacedItem);
        const auto started = std::chrono::steady_clock::now();
        const auto pacedBody = body(get(pacedUrl, "GET", "", &pacedRoute));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(pacedBody.size() == pacedCopy->size());
        assert(elapsed >= 2800ms && elapsed < 5s);
        std::cout << "PASS packed 8 s row at twice its average wire rate, elapsed="
                  << std::chrono::duration<double>(elapsed).count() << " s\n";
        const auto remote = store.create({"remote", 44100, frames, 1, 2, 24, PromiseKind::UpperBound});
        const auto remoteUrl = route.publish("Study", 1, 1, remote);
        assert(remoteUrl == "/yeney/q/session/1/1/remote.wav");
        assert(get(remoteUrl, "HEAD").find("Content-Type: audio/wav\r\n") != std::string::npos);
        std::cout << "PASS FLAC HTTP ranges, HEAD, path and upper-bound WAV\n";
    }
    std::filesystem::remove_all(temp);
}
