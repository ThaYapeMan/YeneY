// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_native_decoder.h"
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
namespace {
class BytesDecoder final : public yeney::Decoder {
    mutable std::mutex mutex;
    std::deque<uint8_t> bytes;
    bool finished = false;
public:
    size_t feed(const uint8_t *data, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex);
        size = std::min(size, size_t(32) - bytes.size());
        bytes.insert(bytes.end(), data, data + size);
        return size;
    }
    void finish() override { std::lock_guard<std::mutex> lock(mutex); finished = true; }
    size_t take(yeney::Frame *out, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex);
        size = std::min(size, bytes.size());
        for (size_t i = 0; i < size; ++i) {
            out[i].left = out[i].right = bytes.front();
            bytes.pop_front();
        }
        return size;
    }
    bool format(yeney::Format &) const override { return true; }
    bool done() const override { std::lock_guard<std::mutex> lock(mutex); return finished && bytes.empty(); }
    std::string error() const override { return {}; }
    size_t inputBuffered() const override { std::lock_guard<std::mutex> lock(mutex); return bytes.size(); }
    size_t outputBuffered() const override { return inputBuffered(); }
};
}
namespace yeney {
std::unique_ptr<Decoder> makeDecoder(const DecoderConfig &) { return std::make_unique<BytesDecoder>(); }
}
int main() {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    assert(server >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    assert(listen(server, 2) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(server, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    unsigned port = ntohs(address.sin_port);
    std::string native(65536, 'n');
    for (size_t n = 0; n < native.size(); ++n) native[n] = char(n % 251);
    std::thread peer([&] {
        int fd = accept(server, nullptr, nullptr);
        assert(fd >= 0);
        std::string request;
        char data[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto n = recv(fd, data, sizeof(data), 0);
            assert(n > 0);
            request.append(data, n);
        }
        assert(request.find("GET /music/13/download HTTP/1.1\r\n") == 0);
        assert(request.find("TimeSeekRange.dlna.org: npt=1.250000-\r\n") != std::string::npos);
        std::string reply = "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                            std::to_string(native.size()) + "\r\n\r\n" + native;
        for (size_t sent = 0; sent < reply.size();) {
            auto n = send(fd, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
            assert(n > 0);
            sent += size_t(n);
        }
        close(fd);
    });
    yeney::DecoderConfig opaque;
    opaque.codec = 'f';
    auto decoder = bridge::nativeQueueDecoder(opaque, "127.0.0.1", "13", 1250, {}, port);
    const uint8_t stale[] = {1, 2, 3};
    assert(decoder->feed(stale, sizeof(stale)) == sizeof(stale));
    std::string actual;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (actual.size() < 3) {
        yeney::Frame frames[32];
        auto n = decoder->take(frames, 32);
        for (size_t i = 0; i < n; ++i) actual.push_back(char(frames[i].left));
        assert(actual.size() <= 3); // Immutable input cannot bypass core credit.
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    yeney::Frame noCredit[32];
    assert(decoder->take(noCredit, 32) == 0);
    decoder->finish(); // Shared-stream EOF must not truncate the bound source.
    while (!decoder->done()) {
        yeney::Frame frames[32];
        auto n = decoder->take(frames, 32);
        for (size_t i = 0; i < n; ++i) actual.push_back(char(frames[i].left));
        assert(decoder->error().empty());
        assert(decoder->inputBuffered() <= 32);
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(actual == native);
    decoder.reset();
    peer.join();
    std::string wave = "RIFFxxxxWAVE";
    auto put32 = [](std::string &s, uint32_t n) {
        for (unsigned i = 0; i < 4; ++i) s.push_back(char(n >> (8*i)));
    };
    auto chunk = [&](const std::string &tag, const std::string &body) {
        wave += tag;
        put32(wave, uint32_t(body.size()));
        wave += body;
        if (body.size() & 1) wave.push_back('\0');
    };
    chunk("fmt ", std::string(16, '\0'));
    chunk("JUNK", "odd");
    chunk("data", native);
    chunk("LIST", "metadata");
    const auto riffBytes = uint32_t(wave.size()-8);
    for (unsigned i = 0; i < 4; ++i) wave[4+i] = char(riffBytes >> (8*i));
    std::thread wavePeer([&] {
        int fd = accept(server, nullptr, nullptr);
        assert(fd >= 0);
        std::string request;
        char data[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto n = recv(fd, data, sizeof(data), 0);
            assert(n > 0);
            request.append(data, n);
        }
        assert(request.find("GET /music/15/download HTTP/1.1\r\n") == 0);
        std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: " +
                            std::to_string(wave.size()) + "\r\n\r\n" + wave;
        for (size_t sent = 0; sent < reply.size();) {
            auto n = send(fd, reply.data() + sent, std::min<size_t>(13, reply.size()-sent), MSG_NOSIGNAL);
            assert(n > 0);
            sent += size_t(n);
        }
        close(fd);
    });
    decoder = bridge::nativeQueueDecoder({}, "127.0.0.1", "15", 0, {}, port);
    std::vector<uint8_t> rawCredit(native.size(), 255);
    assert(decoder->feed(rawCredit.data(), rawCredit.size()) == rawCredit.size());
    decoder->finish();
    actual.clear();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!decoder->done()) {
        yeney::Frame frames[32];
        auto n = decoder->take(frames, 32);
        for (size_t i = 0; i < n; ++i) actual.push_back(char(frames[i].left));
        assert(decoder->error().empty());
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(actual == native); // RIFF/fmt/JUNK/LIST never enter the PCM decoder.
    decoder.reset();
    wavePeer.join();
    std::atomic<bool> accepted{false}, release{false};
    std::thread stalled([&] {
        int fd = accept(server, nullptr, nullptr);
        assert(fd >= 0);
        accepted = true;
        while (!release) std::this_thread::yield();
        close(fd);
    });
    decoder = bridge::nativeQueueDecoder(opaque, "127.0.0.1", "14", 0, {}, port);
    while (!accepted) std::this_thread::yield();
    auto start = std::chrono::steady_clock::now();
    decoder.reset();
    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(200));
    release = true;
    stalled.join();
    close(server);
    std::cout << "PASS immutable native source identity, seek, EOF, input credit, WAV raw representation, backpressure and cancellation\n";
}
