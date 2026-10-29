// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wav_writer.h"
#include "queue_item_store.h"
#include "upnp/stream_server.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cerrno>
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
    struct Diagnostic {
        upnp::StreamRequest &request;
        std::shared_ptr<Item> item;
        unsigned status = 0;
        uint64_t length = 0, served = 0, bodyStart = UINT64_MAX, backpressure = 0;
        std::string ending = "cancelled";
        ~Diagnostic() {
            const auto sent = request.connectionSentBytes();
            if (bodyStart != UINT64_MAX && sent != UINT64_MAX && sent >= bodyStart) served = sent-bodyStart;
            // Only a valid numeric Range is logged. Never include URLs or tokens.
            auto range = request.header("Range");
            if (range.find_first_not_of("bytes=0123456789-, ") != std::string::npos || range.size() > 128)
                range = "invalid";
            std::printf("yeney: queue HTTP method=%s item=%s range=%s status=%u content_length=%llu produced_bytes=%llu served_bytes=%llu end=%s backpressure_retries=%llu\n",
                request.method() == upnp::StreamRequest::Method::Get ? "GET" :
                request.method() == upnp::StreamRequest::Method::Head ? "HEAD" : "OTHER",
                item->descriptor().occurrence.c_str(), range.empty() ? "none" : range.c_str(), status,
                (unsigned long long)length, (unsigned long long)(44 + item->producedFrames()*6),
                (unsigned long long)served, ending.c_str(), (unsigned long long)backpressure);
        }
    } diagnostic{request, item};
    if (request.method() != upnp::StreamRequest::Method::Get &&
        request.method() != upnp::StreamRequest::Method::Head) {
        diagnostic.status = 405; diagnostic.ending = "complete";
        request.reply(405);
        return true;
    }
    auto length = item->contentLength();
    if (!length) {
        diagnostic.status = 503; diagnostic.ending = "complete";
        request.reply(503);
        return true;
    } // sealed assets are unpublished
    ByteRange range;
    try {
        range = wavRange(request.header("Range"), *length);
    } catch (const std::exception &) {
        diagnostic.status = 416; diagnostic.ending = "complete";
        const auto wire = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                          std::to_string(*length) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        request.send(wire.data(), wire.size());
        return true;
    }
    diagnostic.status = range.partial ? 206 : 200;
    diagnostic.length = range.last - range.first + 1;
    auto wire =
        std::string("HTTP/1.1 ") + (range.partial ? "206 Partial Content" : "200 OK") +
        "\r\nContent-Type: audio/wav\r\nContent-Length: " + std::to_string(range.last - range.first + 1) +
        "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n";
    if (range.partial)
        wire += "Content-Range: bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) +
                "/" + std::to_string(*length) + "\r\n";
    wire += "\r\n";
    auto send = [&](const char *bytes, size_t size) {
        while (size && !request.aborted() && !request.peerClosed()) {
            const auto before = request.connectionSentBytes();
            if (request.send(bytes,size)) return true;
            const auto error = errno;
            const auto after = request.connectionSentBytes();
            if ((error != EAGAIN && error != EWOULDBLOCK && error != ETIMEDOUT) ||
                before == UINT64_MAX || after == UINT64_MAX || after < before || after-before > size) {
                errno = error;
                return false;
            }
            // A bounded send deadline is a backpressure observation, not EOF.
            // The exact socket count fences a partially accepted chunk so no
            // bytes are repeated when the healthy speaker starts reading again.
            const auto accepted = size_t(after-before);
            bytes += accepted;size -= accepted;
            (void)item->contentLength(); // intentional flush/eviction cancels the wait
            if (!diagnostic.backpressure++)
                std::printf("yeney: queue HTTP backpressure item=%s waiting=receiver count_bytes=%llu\n",
                            item->descriptor().occurrence.c_str(),(unsigned long long)after);
        }
        return !size;
    };
    if (!send(wire.data(), wire.size())) {
        diagnostic.ending = errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? "timeout" : "speaker-closed";
        return true;
    }
    diagnostic.bodyStart = request.connectionSentBytes();
    if (request.method() == upnp::StreamRequest::Method::Head) {
        diagnostic.ending = "complete";
        return true;
    }
    auto reader = item->reader(range.first);
    uint64_t left = range.last - range.first + 1;
    auto deadline = std::chrono::steady_clock::now() + wait;
    diagnostic.ending = "speaker-closed";
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
            if (!n) { diagnostic.ending = "early-EOF"; break; }
            if (!send(bytes, n)) {
                diagnostic.ending = errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? "timeout" : "speaker-closed";
                break;
            }
            diagnostic.served += n;
            left -= n;
            deadline = std::chrono::steady_clock::now() + wait;
        }
        if (!left) diagnostic.ending = "complete";
        else if (request.aborted()) diagnostic.ending = "cancelled";
    } catch (const std::exception &e) {
        diagnostic.ending = dynamic_cast<const ReadTimeout *>(&e) ? "timeout" : "cancelled";
        std::printf("yeney: queue HTTP read closed item=%s offset=%llu reason=%s\n",
                    item->descriptor().occurrence.c_str(),
                    static_cast<unsigned long long>(reader.position()), e.what());
        request.disconnect();
    }
    return true;
}
} // namespace queue_audio
