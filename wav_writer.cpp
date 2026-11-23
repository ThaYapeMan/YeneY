// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wav_writer.h"
#include "flac_writer.h"
#include "consumption_diagnostic.h"
#include "queue_item_store.h"
#include "upnp/stream_server.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#include <thread>
namespace queue_audio {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

unsigned millisecondsSetting(const char *key, unsigned fallback, unsigned maximum) {
    if (const auto *value = std::getenv(key)) {
        char *end;
        const auto number = std::strtoul(value, &end, 10);
        if (*value >= '0' && *value <= '9' && !*end && number <= maximum)
            return static_cast<unsigned>(number);
        std::printf("yeney: queue invalid %s; using %u\n", key, fallback);
    }
    return fallback;
}

class ByteBudget {
    const double perSecond, burst;
    double available;
    Clock::time_point updated = Clock::now();

public:
    ByteBudget(double bytesPerSecond, const Pacing &pacing)
        : perSecond(bytesPerSecond * pacing.rate),
          burst(bytesPerSecond * (pacing.freeMs / 1000.0)), available(burst) {}

    size_t chunkLimit() const {
        // With zero free bytes each chunk earns its budget from zero.
        return static_cast<size_t>(std::max(1.0, burst ? burst : perSecond / 100));
    }

    template<class Active>
    bool take(size_t size, Clock::duration &waited, Active active) {
        auto now = Clock::now();
        auto refill = [&](double capacity) {
            available = std::min(capacity, available +
                std::chrono::duration<double>(now - updated).count() * perSecond);
            updated = now;
        };
        refill(burst);
        const auto start = now;
        while (available < size) {
            if (!active())
                return false;
            const auto delay = std::chrono::ceil<Clock::duration>(
                std::chrono::duration<double>((size - available) / perSecond));
            std::this_thread::sleep_for(std::min(delay, Clock::duration(5ms)));
            now = Clock::now();
            waited += now - updated;
            refill(std::max(burst, double(size)));
        }
        available -= size;
        return start == now || active();
    }
};
} // namespace

Pacing Pacing::environment() {
    Pacing settings;
    if (const auto *value = std::getenv("YENEY_QUEUE_PACING")) {
        if (!std::strcmp(value, "off"))
            settings.enabled = false;
        else if (std::strcmp(value, "on"))
            std::printf("yeney: queue invalid YENEY_QUEUE_PACING; using on\n");
    }
    if (const auto *value = std::getenv("YENEY_QUEUE_PACE_RATE")) {
        char *end;
        const auto rate = std::strtod(value, &end);
        if (end != value && !*end && rate >= 1 && rate <= 8)
            settings.rate = rate;
        else
            std::printf("yeney: queue invalid YENEY_QUEUE_PACE_RATE; using 2.00\n");
    }
    settings.freeMs = millisecondsSetting("YENEY_QUEUE_PACE_FREE_MS", settings.freeMs, 30000);
    settings.tailMs = millisecondsSetting("YENEY_QUEUE_PACE_TAIL_MS", settings.tailMs, 120000);
    return settings;
}

void Pacing::log() const {
    std::printf("yeney: queue pacing=%s rate=%.2f free_ms=%u tail_ms=%u\n",
                enabled ? "on" : "off", rate, freeMs, tailMs);
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
namespace {
class FlacReader {
    FlacLayout layout;
    uint64_t offset, frameIndex;
    Reader pcm;
    std::array<char, FlacLayout::headerSize> header;
    std::array<char, FlacLayout::blockSize * 6> input;
    size_t filled = 0;
    std::vector<char> frame;

    static uint64_t frameAt(const FlacLayout &layout, uint64_t offset) {
        uint64_t lo = 0, hi = layout.frameCount();
        while (lo + 1 < hi) {
            const auto middle = lo + (hi - lo) / 2;
            if (layout.offset(middle) > offset)
                hi = middle;
            else
                lo = middle;
        }
        return lo;
    }

public:
    FlacReader(const Item &item, const FlacLayout &layout, uint64_t offset)
        : layout(layout), offset(offset), frameIndex(frameAt(layout, offset)),
          pcm(item.reader(44 + frameIndex * FlacLayout::blockSize * 6)), header(layout.header()) {}

    size_t read(char *target, size_t limit, std::chrono::milliseconds wait) {
        if (offset < header.size()) {
            const auto count = std::min<uint64_t>(limit, header.size() - offset);
            std::copy_n(header.data() + offset, count, target);
            offset += count;
            return count;
        }

        if (frame.empty()) {
            const auto need = layout.frameSamples(frameIndex) * 6;
            while (filled < need) {
                const auto count = pcm.read(input.data() + filled, need - filled, wait);
                if (!count)
                    throw std::runtime_error("incomplete FLAC frame");
                filled += count;
            }
            frame = layout.frame(frameIndex, input.data());
            filled = 0;
        }
        const auto skip = offset - layout.offset(frameIndex);
        const auto count = std::min<uint64_t>(limit, frame.size() - skip);
        std::copy_n(frame.data() + skip, count, target);
        offset += count;
        if (skip + count == frame.size()) {
            frame.clear();
            ++frameIndex;
        }
        return count;
    }
};
} // namespace

bool serveRow(upnp::StreamRequest &request, const std::shared_ptr<Item> &published, Format format,
              std::chrono::milliseconds wait,
              std::function<std::shared_ptr<Item>(upnp::StreamRequest &)> resolve, bool audioPinned,
              const Pacing &pacing, unsigned bits) {
    auto item = published;
    const auto &description = item->descriptor();
    std::optional<FlacLayout> flac;
    const auto headerSize = format == Format::Flac ? FlacLayout::headerSize : 44;
    if (request.method() == upnp::StreamRequest::Method::Get && !audioPinned) {
        item = Item::retainAudio(item);
        if (!item) { request.reply(503); return true; }
    }
    struct Diagnostic {
        upnp::StreamRequest &request;
        std::shared_ptr<Item> item;
        Format format;
        unsigned bits;
        const std::optional<FlacLayout> &flac;
        unsigned status = 0;
        uint64_t length = 0, served = 0, bodyStart = UINT64_MAX, backpressure = 0;
        std::string ending = "cancelled";
        bool paced = false;
        Clock::duration paceWait{};
        ~Diagnostic() {
            const auto sent = request.connectionSentBytes();
            if (bodyStart != UINT64_MAX && sent != UINT64_MAX && sent >= bodyStart) served = sent-bodyStart;
            // Only a valid numeric Range is logged. Never include URLs or tokens.
            auto range = request.header("Range");
            if (range.find_first_not_of("bytes=0123456789-, ") != std::string::npos || range.size() > 128)
                range = "invalid";
            const auto frames = item->producedFrames();
            const auto produced = flac
                ? flac->offset(frames == flac->totalSamples() ? flac->frameCount() : frames / FlacLayout::blockSize)
                : 44 + frames * 6;
            std::printf("yeney: queue HTTP method=%s item=%s range=%s form=%s bits=%u status=%u content_length=%llu produced_bytes=%llu served_bytes=%llu end=%s backpressure_retries=%llu paced=%d pace_wait_ms=%lld\n",
                request.method() == upnp::StreamRequest::Method::Get ? "GET" :
                request.method() == upnp::StreamRequest::Method::Head ? "HEAD" : "OTHER",
                item->descriptor().occurrence.c_str(), range.empty() ? "none" : range.c_str(), formatName(format), bits, status,
                (unsigned long long)length, (unsigned long long)produced,
                (unsigned long long)served, ending.c_str(), (unsigned long long)backpressure,
                paced, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(paceWait).count()));
        }
    } diagnostic{request, item, format, bits, flac};
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
    const auto wavSize = *length;
    const auto frames = (wavSize - 44) / 6;
    if (format == Format::Flac) {
        flac.emplace(frames, description.rate, bits);
        length = flac->size();
    }
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
        "\r\nContent-Type: audio/" + formatName(format) + "\r\nContent-Length: " + std::to_string(range.last - range.first + 1) +
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
    if (resolve && range.last >= headerSize) {
        // The immutable declaration has already reached the speaker. HEAD and
        // rejected ranges never select LMS; an abandoned GET may end here with
        // headers only. The room resolver owns settle/selection, never this HTTP
        // worker. It must observe peer close and its bounded startup deadline.
        std::shared_ptr<Item> produced;
        try {
            produced = resolve(request);
            if (produced && (produced->room() != published->room() ||
                produced->descriptor().rate != published->descriptor().rate ||
                produced->contentLength() != wavSize ||
                (bits == 16 && produced->descriptor().bits != 16) ||
                produced->descriptor().kind != published->descriptor().kind)) {
                diagnostic.ending = "declaration-mismatch";
                request.disconnect();
                return true;
            }
        } catch (const std::exception &) {
            diagnostic.ending = "resolution-failed";
            request.disconnect();
            return true;
        }
        if (!produced || request.aborted() || request.peerClosed()) {
            diagnostic.ending = "abandoned";
            return true;
        }
        item = std::move(produced);
        diagnostic.item = item;
    }
    bridge::ConsumptionDiagnostic consumption(request, item->room(), "queue");
    std::optional<Reader> wavReader;
    std::optional<FlacReader> flacReader;
    if (flac) flacReader.emplace(*item, *flac, range.first);
    else wavReader.emplace(item->reader(range.first));
    const double playRate = frames ? double(*length) * description.rate / frames : double(description.rate) * 6;
    const auto tailBytes = static_cast<uint64_t>(std::min<double>(*length, playRate * pacing.tailMs / 1000));
    const auto tailStart = *length - tailBytes;
    diagnostic.paced = pacing.enabled && range.first < tailStart;
    ByteBudget budget(playRate, pacing);
    auto active = [&] {
        if (request.aborted() || request.peerClosed())
            return false;
        (void)item->contentLength();
        return true;
    };
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
            const auto offset = range.last - left + 1;
            const bool paced = pacing.enabled && offset < tailStart;
            try {
                const auto limit = paced
                    ? std::min<uint64_t>({sizeof(bytes), budget.chunkLimit(), tailStart - offset})
                    : sizeof(bytes);
                const auto count = std::min<uint64_t>(left, limit);
                if (!active()) break;
                n = flacReader ? flacReader->read(bytes, count, 100ms)
                               : wavReader->read(bytes, count, 100ms);
            } catch (const ReadTimeout &) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw;
                continue;
            }
            if (!n) { diagnostic.ending = "early-EOF"; break; }
            if (paced && !budget.take(n, diagnostic.paceWait, active))
                break;
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
                    static_cast<unsigned long long>(range.last - left + 1), e.what());
        request.disconnect();
    }
    return true;
}
bool serveWav(upnp::StreamRequest &request, const std::shared_ptr<Item> &item, std::chrono::milliseconds wait,
              std::function<std::shared_ptr<Item>(upnp::StreamRequest &)> resolve, bool audioPinned,
              const Pacing &pacing) {
    return serveRow(request, item, Format::Wav, wait, std::move(resolve), audioPinned, pacing);
}
} // namespace queue_audio
