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
struct NativeMovie {
    std::mutex mutex;
    std::condition_variable changed;
    yeney::Bytes movie;
    bool ready = false, cancelled = false;
    yeney::Bytes get() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready || cancelled; });
        return cancelled ? yeney::Bytes{} : std::move(movie);
    }
    void publish(yeney::Bytes value = {}) {
        { std::lock_guard<std::mutex> lock(mutex); if (ready) return; movie = std::move(value); ready = true; }
        changed.notify_all();
    }
    void cancel() {
        { std::lock_guard<std::mutex> lock(mutex); cancelled = true; }
        changed.notify_all();
    }
};
// Inspect only bounded leading box headers. The main response retains its
// original byte offsets; metadata is supplied separately to the core parser.
std::optional<uint64_t> tailMovie(const yeney::Bytes &bytes, uint64_t total, bool &decided) {
    uint64_t cursor = 0;
    while (cursor + 8 <= bytes.size()) {
        auto number = [&](size_t at) {
            return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at+1]) << 16 |
                   uint32_t(bytes[at+2]) << 8 | bytes[at+3];
        };
        uint64_t length = number(size_t(cursor));
        size_t header = 8;
        if (length == 1) {
            if (cursor + 16 > bytes.size()) return {};
            length = uint64_t(number(size_t(cursor+8))) << 32 | number(size_t(cursor+12));
            header = 16;
        }
        if (length < header || cursor > total || length > total-cursor) { decided = true; return {}; }
        const auto *kind = bytes.data()+size_t(cursor)+4;
        if (!std::memcmp(kind, "moov", 4)) { decided = true; return {}; }
        if (!std::memcmp(kind, "mdat", 4)) {
            decided = true;
            return cursor+length < total ? std::optional<uint64_t>(cursor+length) : std::nullopt;
        }
        cursor += length;
    }
    return {};
}
class NativePcmPayload {
    bool enabled, top = true, audio = false, found = false, format = false, wave = true;
    bool soundHeader = false;
    uint64_t soundSkip = 0;
    std::function<void()> provenOrigin;
    std::function<void(uint64_t, unsigned)> provenFormat;
    bool framingMatches = false;
    unsigned expectedWidth = 0, headerWidth = 0, headerBits = 0;
    size_t formatNeed = 0;
    std::string formatPrefix;
    std::optional<uint64_t> commonFrames;
    uint64_t expected = 0, received = 0, left = 0, padding = 0;
    std::string header;
    static uint32_t little(const char *p) {
        const auto *b = reinterpret_cast<const uint8_t *>(p);
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }
public:
    explicit NativePcmPayload(bool rawPcm, unsigned width, std::function<void()> origin,
                              std::function<void(uint64_t, unsigned)> sourceFormat)
        : enabled(rawPcm), provenOrigin(std::move(origin)), provenFormat(std::move(sourceFormat)), expectedWidth(width) {}
    void length(uint64_t bytes) { expected = bytes; }
    template<class Feed> bool feed(const char *bytes, size_t count, Feed consume) {
        received += count;
        if (!enabled) return consume(bytes, count);
        // The immutable download begins at byte zero. Strip container framing,
        // preserving LMS's raw PCM representation and its byte order.
        while (count) {
            if (formatNeed) {
                const auto n = std::min(count, std::min<size_t>(size_t(left), formatNeed-formatPrefix.size()));
                formatPrefix.append(bytes, n); bytes += n; count -= n; left -= n;
                if (formatPrefix.size() < formatNeed) continue;
                const auto *b = reinterpret_cast<const uint8_t *>(formatPrefix.data());
                if (wave) {
                    const auto kind = unsigned(b[0]) | unsigned(b[1]) << 8;
                    if (kind == 1 || (kind == 65534 && formatPrefix.size() >= 40 && b[24] == 1 && !b[25])) {
                        headerWidth = unsigned(b[12]) | unsigned(b[13]) << 8;
                        headerBits = unsigned(b[14]) | unsigned(b[15]) << 8;
                    }
                } else {
                    const auto channels = unsigned(b[0]) << 8 | b[1];
                    headerBits = unsigned(b[6]) << 8 | b[7];
                    headerWidth = channels * ((headerBits+7)/8);
                    commonFrames = uint32_t(b[2]) << 24 | uint32_t(b[3]) << 16 | uint32_t(b[4]) << 8 | b[5];
                }
                formatNeed = 0; formatPrefix.clear();
                if (!left && padding) { left = padding; padding = 0; }
                continue;
            }
            if (soundHeader) {
                const auto n = std::min(count, size_t(8) - header.size());
                header.append(bytes, n); bytes += n; count -= n;
                if (header.size() != 8) continue;
                const auto *b = reinterpret_cast<const uint8_t *>(header.data());
                soundSkip = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
                if (soundSkip > left - 8) throw std::runtime_error("native AIFF sound offset");
                left -= 8; soundHeader = false; audio = true; header.clear();
                const uint64_t available = expected >= received-count ? expected-(received-count) : 0;
                if (left > available) { left = available; framingMatches = false; padding = 0; }
                if (soundSkip > left) throw std::runtime_error("native AIFF response offset");
                if (framingMatches) {
                    provenOrigin();
                    if (headerWidth && headerWidth == expectedWidth && commonFrames &&
                        *commonFrames <= UINT64_MAX/headerWidth && *commonFrames*headerWidth == left-soundSkip)
                        provenFormat(*commonFrames, headerBits);
                }
                continue;
            }
            if (soundSkip) {
                const auto n = size_t(std::min<uint64_t>(soundSkip, count));
                soundSkip -= n; left -= n; bytes += n; count -= n;
                continue;
            }
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
                wave = !header.compare(0, 4, "RIFF") && !header.compare(8, 4, "WAVE");
                const bool aiff = !header.compare(0, 4, "FORM") &&
                    (!header.compare(8, 4, "AIFF") || !header.compare(8, 4, "AIFC"));
                const auto *b = reinterpret_cast<const uint8_t *>(header.data()+4);
                const uint32_t length = wave ? little(header.data()+4) :
                    uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
                if (!wave && !aiff) throw std::runtime_error("native PCM container framing");
                framingMatches = uint64_t(length)+8 == expected;
                top = false;
            } else {
                const auto *b = reinterpret_cast<const uint8_t *>(header.data()+4);
                left = wave ? little(header.data()+4) :
                    uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
                padding = left & 1;
                audio = header.compare(0, 4, wave ? "data" : "SSND") == 0;
                if (audio) {
                    if (found || !format) throw std::runtime_error("native WAV data order");
                    found = true;
                    if (wave) {
                        const uint64_t available = expected >= received-count ? expected-(received-count) : 0;
                        if (left > available) { left = available; framingMatches = false; padding = 0; }
                        if (framingMatches) {
                            provenOrigin();
                            if (headerWidth && headerWidth == expectedWidth && !(left%headerWidth))
                                provenFormat(left/headerWidth, headerBits);
                        }
                    } else {
                        if (left < 8) throw std::runtime_error("native AIFF sound framing");
                        soundHeader = true; audio = false;
                    }
                } else if (!header.compare(0, 4, wave ? "fmt " : "COMM")) {
                    if (format || left < (wave ? 16u : 18u)) throw std::runtime_error("native PCM format framing");
                    format = true;
                    formatNeed = wave && left >= 40 ? 40 : wave ? 16 : 18;
                }
            }
            header.clear();
        }
        return true;
    }
    void finish() const {
        if (enabled && (top || !found || !format || left || padding || soundHeader || soundSkip || !header.empty() || received != expected))
            throw std::runtime_error("native WAV truncated chunks");
    }
};
class NativeDecoder final : public yeney::Decoder {
    std::shared_ptr<std::atomic_bool> pcmOriginKnown = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<NativeMovie> movie = std::make_shared<NativeMovie>();
    std::shared_ptr<std::atomic<uint64_t>> pcmFrames = std::make_shared<std::atomic<uint64_t>>(UINT64_MAX);
    std::shared_ptr<std::atomic_bool> fileFromStart = std::make_shared<std::atomic_bool>(false);
    static yeney::DecoderConfig withOrigin(yeney::DecoderConfig config,
                                          std::shared_ptr<std::atomic_bool> proof,
                                          std::shared_ptr<NativeMovie> metadata, std::shared_ptr<std::atomic<uint64_t>> frames, std::shared_ptr<std::atomic_bool> original, bool preload) {
        if (config.codec == 'p') config.pcmSourceStart = [proof]() -> std::optional<uint64_t> {
            return proof->load() ? std::optional<uint64_t>(0) : std::nullopt;
        };
        if (config.codec == 'p') config.pcmSourceFrames = [frames]() -> std::optional<uint64_t> {
            const auto count = frames->load();
            return count == UINT64_MAX ? std::nullopt : std::optional<uint64_t>(count);
        };
        if (config.codec == 'm') config.mp3FromStart = [original] { return original->load(); };
        if (preload) config.mp4Metadata = [metadata] { return metadata->get(); };
        return config;
    }
    const char codec;
    std::atomic<unsigned> pcmBits{0};
    std::function<void()> depth16;
    mutable bool reportedDepth = false;
    std::atomic<bool> stop{false};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::atomic<uint64_t> inputProgress{0};
    std::function<void()> notifyPlayer;
    std::string failure;
    std::unique_ptr<yeney::Decoder> decoder;
    std::thread worker;
    std::function<void(const std::string &)> log;
    std::atomic<uint64_t> ingested{0};
    std::atomic<uint64_t> allowance{0};
    std::atomic<bool> sharedEof{false};
    bool announced = false;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    void inputChanged() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++inputProgress;
        }
        changed.notify_one();
    }

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
    yeney::Bytes fetchMovie(const std::string &host, const std::string &track,
                            unsigned port, uint64_t start, uint64_t total) {
        constexpr uint64_t cap = 32ull * 1024 * 1024;
        if (start >= total || total-start > cap) return {};
        struct Socket { int fd = -1; ~Socket() { if (fd >= 0) ::close(fd); } } socket;
        socket.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (socket.fd < 0) throw std::runtime_error("metadata socket");
        sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &peer.sin_addr) != 1) return {};
        if (::connect(socket.fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) < 0) {
            if (errno != EINPROGRESS || !wait(socket.fd, POLLOUT)) return {};
            int error = 0; socklen_t length = sizeof(error);
            if (getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) return {};
        }
        const auto request = "GET /music/"+track+"/download HTTP/1.1\r\nHost: "+host+":"+
            std::to_string(port)+"\r\nRange: bytes="+std::to_string(start)+"-"+
            std::to_string(total-1)+"\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        while (!stop && sent < request.size()) {
            if (!wait(socket.fd, POLLOUT)) return {};
            auto n = ::send(socket.fd, request.data()+sent, request.size()-sent, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0) return {};
            sent += size_t(n);
        }
        std::string headers;
        yeney::Bytes bytes;
        bool body = false;
        while (!stop && (!body || bytes.size() < total-start)) {
            if (!wait(socket.fd, POLLIN)) return {};
            char chunk[16384]; auto n = ::recv(socket.fd, chunk, sizeof(chunk), 0);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0) return {};
            const char *data = chunk; size_t count = size_t(n);
            if (!body) {
                headers.append(chunk, count); auto split = headers.find("\r\n\r\n");
                if (split == std::string::npos) {
                    if (headers.size() > 65536) return {};
                    continue;
                }
                if (headers.size() < 12 || headers.substr(9, 3) != "206") return {};
                auto fields = headers.substr(0, split);
                for (auto &c : fields) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
                const auto length = fields.find("\r\ncontent-length:");
                const auto expected = "\r\ncontent-range: bytes "+std::to_string(start)+"-"+
                    std::to_string(total-1)+"/"+std::to_string(total);
                const auto range = fields.find(expected);
                if (length == std::string::npos || std::stoull(fields.substr(length+17)) != total-start ||
                    range == std::string::npos ||
                    (range+expected.size() != fields.size() && fields[range+expected.size()] != '\r') ||
                    fields.find("\r\ntransfer-encoding:") != std::string::npos)
                    return {};
                body = true; data = headers.data()+split+4; count = headers.size()-split-4;
            }
            if (count > total-start-bytes.size()) return {};
            bytes.insert(bytes.end(), data, data+count);
            if (!headers.empty()) headers.clear();
        }
        for (size_t cursor = 0; cursor+8 <= bytes.size();) {
            auto number = [&](size_t at) { return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at+1]) << 16 |
                uint32_t(bytes[at+2]) << 8 | bytes[at+3]; };
            uint64_t length = number(cursor); size_t header = 8;
            if (length == 1) {
                if (cursor+16 > bytes.size()) return {};
                length = uint64_t(number(cursor+8)) << 32 | number(cursor+12); header = 16;
            }
            if (length < header || length > bytes.size()-cursor) return {};
            if (!std::memcmp(bytes.data()+cursor+4, "moov", 4)) {
                if (log) log("metadata_range start="+std::to_string(start)+" bytes="+std::to_string(length));
                return yeney::Bytes(bytes.begin()+cursor+header, bytes.begin()+cursor+size_t(length));
            }
            cursor += size_t(length);
        }
        return {};
    }
    void produce(std::string host, std::string track, unsigned offsetMs, unsigned port, bool rawPcm, bool preload, unsigned pcmWidth) {
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
            uint64_t supplied = 0, responseBytes = 0;
            bool inspected = !preload, independentInput = false;
            yeney::Bytes leading;
            bool fullResponse = false;
            NativePcmPayload representation(rawPcm, pcmWidth, [this, &fullResponse, offsetMs] {
                if (fullResponse && !offsetMs) pcmOriginKnown->store(true);
            }, [this, &fullResponse, offsetMs](uint64_t frames, unsigned bits) {
                if (fullResponse && !offsetMs) {
                    pcmBits.store(bits);
                    pcmFrames->store(frames);
                }
            });
            auto feed = [&](const char *data, size_t bytes) {
                size_t consumed = 0;
                while (!stop && consumed < bytes) {
                    const auto progress = inputProgress.load();
                    const auto credit = allowance.load();
                    const size_t permitted = (sharedEof || independentInput) ? bytes - consumed :
                        size_t(std::min<uint64_t>(bytes - consumed, credit > supplied ? credit - supplied : 0));
                    auto count = permitted ? decoder->feed(
                        reinterpret_cast<const uint8_t *>(data + consumed), permitted) : 0;
                    consumed += count;
                    supplied += count;
                    if (!count) {
                        if (!decoder->error().empty()) return false;
                        std::unique_lock<std::mutex> lock(mutex);
                        changed.wait(lock, [this, progress] {
                            return stop.load() || inputProgress.load() != progress;
                        });
                    }
                }
                return !stop.load();
            };
            while (!stop) {
                if (body && !remaining) {
                    if (!inspected) {
                        movie->publish();
                        if (!representation.feed(reinterpret_cast<const char *>(leading.data()), leading.size(), feed)) return;
                    }
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
                    responseBytes = remaining;
                    fullResponse = headers.substr(9, 3) == "200";
                    fileFromStart->store(fullResponse && !offsetMs);
                    representation.length(remaining);
                    body = true;
                    data = headers.data() + split + 4;
                    bytes = headers.size() - split - 4;
                }
                if (bytes > remaining) throw std::runtime_error("native source excess bytes");
                ingested += bytes;
                if (!inspected) {
                    leading.insert(leading.end(), data, data+bytes);
                    bool decided = false;
                    const auto tail = tailMovie(leading, responseBytes, decided);
                    if (decided || leading.size() >= 65536) {
                        inspected = true;
                        yeney::Bytes metadata;
                        if (tail) {
                            try { metadata = fetchMovie(host, track, port, *tail, responseBytes); }
                            catch (const std::exception &) { if (log) log("metadata_range unavailable"); }
                        }
                        // A server without range support must not pace this
                        // immutable download by unrelated shared-stream credits.
                        if (tail && metadata.empty()) independentInput = true;
                        movie->publish(std::move(metadata));
                        if (!representation.feed(reinterpret_cast<const char *>(leading.data()), leading.size(), feed)) return;
                        leading.clear();
                    }
                } else if (!representation.feed(data, bytes, feed)) return;
                remaining -= bytes;
                if (!headers.empty()) headers.clear();
            }
        } catch (const std::exception &e) {
            movie->publish();
            if (!stop) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    failure = e.what();
                }
                if (notifyPlayer)
                    notifyPlayer();
            }
        }
    }
public:
    NativeDecoder(const yeney::DecoderConfig &config, const std::string &host,
                  const std::string &track, unsigned offsetMs,
                  std::function<void(const std::string &)> logger, unsigned port, std::function<void()> depthProof)
        : codec(config.codec), depth16(std::move(depthProof)), notifyPlayer(config.notifyProgress), log(std::move(logger)) {
        auto workerConfig =
            withOrigin(config, pcmOriginKnown, movie, pcmFrames, fileFromStart, config.codec == 'l' && !offsetMs);
        workerConfig.notifyProgress = [this] {
            inputChanged();
            if (notifyPlayer)
                notifyPlayer();
        };
        decoder = yeney::makeDecoder(workerConfig);
        worker = std::thread([this, host, track, offsetMs, port, rawPcm = config.codec == 'p',
                              preload = config.codec == 'l' && !offsetMs,
                              pcmWidth = config.size >= '0' && config.size <= '3' &&
                                  config.channels >= '1' && config.channels <= '2' ?
                                  unsigned(config.size-'0'+1)*unsigned(config.channels-'0') : 0] {
            produce(host, track, offsetMs, port, rawPcm, preload, pcmWidth);
        });
    }
    ~NativeDecoder() override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        movie->cancel();
        changed.notify_one();
        worker.join();
        // The decoder worker owns its callback until it has joined.
        decoder.reset();
    }
    bool hasIndependentEncodedSource() const override { return true; }
    size_t feed(const uint8_t *, size_t bytes) override {
        allowance += bytes;
        if (bytes)
            inputChanged();
        return bytes;
    }
    void finish() override {
        sharedEof = true; // The immutable HTTP source still owns its own EOF.
        inputChanged();
    }
    size_t take(yeney::Frame *frames, size_t count) override {
        auto n = decoder->take(frames, count);
        if (n && !announced) {
            announced = true;
            if (log) log("first_pcm ingested_bytes=" + std::to_string(ingested.load()));
        }
        return n;
    }
    bool sourceStart(uint64_t &frame) const override { return decoder->sourceStart(frame); }
    uint64_t warmupFrames() const override { return decoder->warmupFrames(); }
    bool format(yeney::Format &f) const override {
        const bool ready = decoder->format(f);
        // FLAC and ALAC report header depth through the decoder. The PCM
        // decoder uses negotiated framing, so also require the container proof.
        if (ready && depth16 && !reportedDepth && fileFromStart->load() && f.bits == 16 &&
            !decoder->processed() && (codec == 'f' || codec == 'l' || (codec == 'p' && pcmBits == 16))) {
            reportedDepth = true;
            depth16();
        }
        return ready;
    }
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
        std::function<void(const std::string &)> log, unsigned port, std::function<void()> depth16) {
    if (track.empty() || track.find_first_not_of("0123456789") != std::string::npos || !port || port > 65535)
        throw std::invalid_argument("invalid native source endpoint");
    return std::make_unique<NativeDecoder>(config, host, track, offsetMs, std::move(log), port, std::move(depth16));
}
}
