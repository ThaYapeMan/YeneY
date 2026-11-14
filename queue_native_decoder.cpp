// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_native_decoder.h"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
namespace bridge {
namespace {
class NativeWavePayload {
    bool enabled, top = true, audio = false, found = false, format = false;
    uint64_t expected = 0, received = 0, left = 0, padding = 0;
    std::string header;
    static uint32_t little(const char *p) {
        const auto *b = reinterpret_cast<const uint8_t *>(p);
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }
public:
    explicit NativeWavePayload(bool rawPcm) : enabled(rawPcm) {}
    void length(uint64_t bytes) { expected = bytes; }
    template<class Feed> bool feed(const char *bytes, size_t count, Feed consume) {
        received += count;
        if (!enabled) return consume(bytes, count);
        // LMS advertises WAV as raw PCM ('p'), removing RIFF chunks. Preserve
        // that representation; a download's header must not add early length
        // information to the pinned core's raw-PCM decoder.
        while (count) {
            if (left) {
                const auto n = size_t(std::min<uint64_t>(left, count));
                if (audio && !consume(bytes, n)) return false;
                left -= n; bytes += n; count -= n;
                if (!left && padding) { left = padding; padding = 0; audio = false; }
                continue;
            }
            const size_t needed = top ? 12 : 8;
            const auto n = std::min(count, needed - header.size());
            header.append(bytes, n); bytes += n; count -= n;
            if (header.size() != needed) continue;
            if (top) {
                if (header.compare(0, 4, "RIFF") || header.compare(8, 4, "WAVE") ||
                    uint64_t(little(header.data()+4)) + 8 != expected)
                    throw std::runtime_error("native WAV RIFF framing");
                top = false;
            } else {
                left = little(header.data()+4);
                padding = left & 1;
                audio = header.compare(0, 4, "data") == 0;
                if (audio) {
                    if (found || !format) throw std::runtime_error("native WAV data order");
                    found = true;
                } else if (!header.compare(0, 4, "fmt ")) {
                    if (format || left < 16) throw std::runtime_error("native WAV format framing");
                    format = true;
                }
            }
            header.clear();
        }
        return true;
    }
    void finish() const {
        if (enabled && (top || !found || !format || left || padding || !header.empty() || received != expected))
            throw std::runtime_error("native WAV truncated chunks");
    }
};
class NativeDecoder final : public yeney::Decoder {
    std::unique_ptr<yeney::Decoder> decoder;
    std::atomic<bool> stop{false};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::string failure;
    std::thread worker;
    std::function<void(const std::string &)> log;
    std::atomic<uint64_t> ingested{0};
    std::atomic<uint64_t> allowance{0};
    std::atomic<bool> sharedEof{false};
    bool announced = false;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    bool wait(int socket, short events) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stop) {
            pollfd p{socket, events, 0};
            int n = ::poll(&p, 1, 20);
            if (n > 0) return true;
            if (n < 0 && errno != EINTR) throw std::runtime_error(std::strerror(errno));
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("native source HTTP timeout");
        }
        return false;
    }
    void produce(std::string host, std::string track, unsigned offsetMs, unsigned port, bool rawPcm) {
        struct Socket {
            int fd = -1;
            ~Socket() { if (fd >= 0) ::close(fd); }
        } socket;
        try {
            sockaddr_in peer{};
            peer.sin_family = AF_INET;
            peer.sin_port = htons(port);
            if (inet_pton(AF_INET, host.c_str(), &peer.sin_addr) != 1)
                throw std::runtime_error("native source requires numeric LMS address");
            socket.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (socket.fd < 0) throw std::runtime_error(std::strerror(errno));
            if (::connect(socket.fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) < 0) {
                if (errno != EINPROGRESS) throw std::runtime_error(std::strerror(errno));
                if (!wait(socket.fd, POLLOUT)) return;
                int error = 0;
                socklen_t size = sizeof(error);
                if (getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
                    throw std::runtime_error("native source connection failed");
            }
            std::string request = "GET /music/" + track + "/download HTTP/1.1\r\nHost: " + host +
                                  ":" + std::to_string(port) + "\r\nConnection: close\r\n";
            if (offsetMs)
                request += "TimeSeekRange.dlna.org: npt=" + std::to_string(offsetMs / 1000.0) + "-\r\n";
            request += "\r\n";
            size_t sent = 0;
            while (!stop && sent < request.size()) {
                if (!wait(socket.fd, POLLOUT)) return;
                auto n = ::send(socket.fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (n <= 0) throw std::runtime_error("native source request failed");
                sent += size_t(n);
            }
            std::string headers;
            bool body = false;
            uint64_t remaining = 0;
            uint64_t supplied = 0;
            NativeWavePayload representation(rawPcm);
            auto feed = [&](const char *data, size_t bytes) {
                size_t consumed = 0;
                while (!stop && consumed < bytes) {
                    const auto credit = allowance.load();
                    const size_t permitted = sharedEof ? bytes - consumed :
                        size_t(std::min<uint64_t>(bytes - consumed, credit > supplied ? credit - supplied : 0));
                    auto count = permitted ? decoder->feed(
                        reinterpret_cast<const uint8_t *>(data + consumed), permitted) : 0;
                    consumed += count;
                    supplied += count;
                    if (!count) {
                        if (!decoder->error().empty()) return false;
                        std::unique_lock<std::mutex> lock(mutex);
                        changed.wait_for(lock, std::chrono::milliseconds(2), [this] { return stop.load(); });
                    }
                }
                return !stop.load();
            };
            while (!stop) {
                if (body && !remaining) {
                    representation.finish();
                    if (log) log("encoded_eof bytes=" + std::to_string(ingested.load()) +
                        " ingest_us=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started).count()));
                    decoder->finish();
                    return;
                }
                if (!wait(socket.fd, POLLIN)) return;
                char buffer[16384];
                auto n = ::recv(socket.fd, buffer, sizeof(buffer), 0);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (n <= 0) throw std::runtime_error("native source truncated");
                const char *data = buffer;
                size_t bytes = size_t(n);
                if (!body) {
                    headers.append(buffer, bytes);
                    auto split = headers.find("\r\n\r\n");
                    if (split == std::string::npos) {
                        if (headers.size() > 65536) throw std::runtime_error("native source headers too large");
                        continue;
                    }
                    if (headers.size() < 12 || (headers.substr(9, 3) != "200" && headers.substr(9, 3) != "206"))
                        throw std::runtime_error("native source HTTP status");
                    std::string fields = headers.substr(0, split);
                    for (auto &c : fields) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                    auto length = fields.find("\r\ncontent-length:");
                    if (length == std::string::npos || fields.find("\r\ntransfer-encoding:") != std::string::npos)
                        throw std::runtime_error("native source HTTP framing");
                    remaining = std::stoull(fields.substr(length + 17));
                    representation.length(remaining);
                    body = true;
                    data = headers.data() + split + 4;
                    bytes = headers.size() - split - 4;
                }
                if (bytes > remaining) throw std::runtime_error("native source excess bytes");
                ingested += bytes;
                if (!representation.feed(data, bytes, feed)) return;
                remaining -= bytes;
                if (!headers.empty()) headers.clear();
            }
        } catch (const std::exception &e) {
            if (!stop) {
                std::lock_guard<std::mutex> lock(mutex);
                failure = e.what();
            }
        }
    }
public:
    NativeDecoder(const yeney::DecoderConfig &config, const std::string &host,
                  const std::string &track, unsigned offsetMs,
                  std::function<void(const std::string &)> logger, unsigned port)
        : decoder(yeney::makeDecoder(config)), log(std::move(logger)) {
        worker = std::thread([this, host, track, offsetMs, port, rawPcm = config.codec == 'p'] {
            produce(host, track, offsetMs, port, rawPcm);
        });
    }
    ~NativeDecoder() override {
        stop = true;
        changed.notify_all();
        worker.join();
    }
    size_t feed(const uint8_t *, size_t bytes) override {
        allowance += bytes;
        changed.notify_all();
        return bytes;
    }
    void finish() override {
        sharedEof = true; // The immutable HTTP source still owns its own EOF.
        changed.notify_all();
    }
    size_t take(yeney::Frame *frames, size_t count) override {
        auto n = decoder->take(frames, count);
        if (n && !announced) {
            announced = true;
            if (log) log("first_pcm ingested_bytes=" + std::to_string(ingested.load()));
        }
        changed.notify_all();
        return n;
    }
    bool sourceStart(uint64_t &frame) const override { return decoder->sourceStart(frame); }
    uint64_t warmupFrames() const override { return decoder->warmupFrames(); }
    bool format(yeney::Format &f) const override { return decoder->format(f); }
    std::optional<uint64_t> exactFrames() const override { return decoder->exactFrames(); }
    bool needsSuccessor() const override { return decoder->needsSuccessor(); }
    void successorRequested() override { decoder->successorRequested(); }
    bool processed() const override { return decoder->processed(); }
    bool done() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return (!failure.empty() && !decoder->outputBuffered()) || decoder->done();
    }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return failure.empty() ? decoder->error() : failure;
    }
    size_t inputBuffered() const override { return decoder->inputBuffered(); }
    size_t outputBuffered() const override { return decoder->outputBuffered(); }
};
}
std::unique_ptr<yeney::Decoder> nativeQueueDecoder(const yeney::DecoderConfig &config,
        const std::string &host, const std::string &track, unsigned offsetMs,
        std::function<void(const std::string &)> log, unsigned port) {
    if (track.empty() || track.find_first_not_of("0123456789") != std::string::npos || !port || port > 65535)
        throw std::invalid_argument("invalid native source endpoint");
    return std::make_unique<NativeDecoder>(config, host, track, offsetMs, std::move(log), port);
}
}
