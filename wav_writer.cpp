// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wav_writer.h"
#include "queue_item_store.h"
#include "upnp/stream_server.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace queue_audio {
uint64_t wavLength(uint64_t frames) {
    if (frames > (uint64_t(UINT32_MAX) - 36) / 6)
        throw std::invalid_argument("WAV exceeds RIFF limit");
    return 44 + frames * 6;
}
std::array<char, 44> wavHeader(uint32_t rate, uint64_t frames) {
    if (!rate || rate > UINT32_MAX / 6)
        throw std::invalid_argument("invalid WAV rate");
    const auto length = wavLength(frames);
    std::array<char, 44> h{};
    std::memcpy(h.data(), "RIFF", 4);
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    std::memcpy(h.data() + 36, "data", 4);
    auto put = [&](unsigned at, uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            h[at + i] = char(value >> (8 * i));
    };
    put(4, length - 8, 4);
    put(16, 16, 4);
    put(20, 1, 2);
    put(22, 2, 2);
    put(24, rate, 4);
    put(28, rate * 6, 4);
    put(32, 6, 2);
    put(34, 24, 2);
    put(40, length - 44, 4);
    return h;
}
ByteRange wavRange(const std::string &text, uint64_t length) {
    if (!length)
        throw std::invalid_argument("empty entity");
    if (text.empty())
        return {0, length - 1, false};
    if (text.compare(0, 6, "bytes=") || text.find(',') != std::string::npos)
        throw std::invalid_argument("unsupported Range");
    const auto dash = text.find('-', 6);
    if (dash == std::string::npos)
        throw std::invalid_argument("invalid Range");
    auto number = [](const std::string &s) -> uint64_t {
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("invalid Range number");
        return std::stoull(s);
    };
    const auto a = text.substr(6, dash - 6), b = text.substr(dash + 1);
    if (a.empty()) {
        auto n = number(b);
        if (!n)
            throw std::invalid_argument("empty suffix");
        return {n >= length ? 0 : length - n, length - 1, true};
    }
    auto first = number(a), last = b.empty() ? length - 1 : std::min(number(b), length - 1);
    if (first >= length || first > last)
        throw std::invalid_argument("unsatisfiable Range");
    return {first, last, true};
}
bool serveWav(upnp::StreamRequest &request, const std::shared_ptr<Item> &item,
              std::chrono::milliseconds wait) {
    if (request.method() != upnp::StreamRequest::Method::Get &&
        request.method() != upnp::StreamRequest::Method::Head) {
        request.reply(405);
        return true;
    }
    auto length = item->contentLength();
    if (!length) {
        request.reply(503);
        return true;
    } // sealed assets are unpublished
    ByteRange range;
    try {
        range = wavRange(request.header("Range"), *length);
    } catch (const std::exception &) {
        const auto wire = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                          std::to_string(*length) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        request.send(wire.data(), wire.size());
        return true;
    }
    auto wire =
        std::string("HTTP/1.1 ") + (range.partial ? "206 Partial Content" : "200 OK") +
        "\r\nContent-Type: audio/wav\r\nContent-Length: " + std::to_string(range.last - range.first + 1) +
        "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n";
    if (range.partial)
        wire += "Content-Range: bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) +
                "/" + std::to_string(*length) + "\r\n";
    wire += "\r\n";
    if (!request.send(wire.data(), wire.size()) || request.method() == upnp::StreamRequest::Method::Head)
        return true;
    auto reader = item->reader(range.first);
    uint64_t left = range.last - range.first + 1;
    auto deadline = std::chrono::steady_clock::now() + wait;
    try {
        while (left && !request.aborted() && !request.peerClosed()) {
            char bytes[8192];
            // Poll the owner connection while waiting for future PCM. The
            // startup deadline applies to first body data; later data has a
            // fresh bounded wait, not a 15 s total track lifetime.
            size_t n;
            try {
                n = reader.read(bytes, std::min<uint64_t>(left, sizeof(bytes)),
                                std::chrono::milliseconds(100));
            } catch (const ReadTimeout &) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw;
                continue;
            }
            if (!n || !request.send(bytes, n))
                break;
            left -= n;
            deadline = std::chrono::steady_clock::now() + wait;
        }
    } catch (...) {
        request.disconnect();
    }
    return true;
}
} // namespace queue_audio
