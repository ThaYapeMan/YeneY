// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_websocket.h"
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <map>
#include <stdexcept>
#include <vector>
#include <cstring>
namespace bridge {
double positionMonotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
// Bounded JSON reader: retain scalar fields while validating the entire message.
class Fields {
    const std::string &text;
    size_t at = 0;
    void space() { while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at; }
    char get() { if (at == text.size()) throw std::runtime_error("json truncated"); return text[at++]; }
    std::string string() {
        if (get() != '"') throw std::runtime_error("json string");
        std::string out;
        while (true) {
            char c = get();
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 32) throw std::runtime_error("json control");
            if (c == '\\') {
                c = get();
                if (c == 'u') {
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = get();
                        const auto digit = std::string("0123456789abcdef").find(char(std::tolower(h)));
                        if (digit == std::string::npos) throw std::runtime_error("json unicode");
                        code = code * 16 + digit;
                    }
                    if (code < 128) out += char(code);
                    else if (code < 2048) { out += char(0xc0 | (code >> 6)); out += char(0x80 | (code & 63)); }
                    else { out += char(0xe0 | (code >> 12)); out += char(0x80 | ((code >> 6) & 63)); out += char(0x80 | (code & 63)); }
                    continue;
                }
                const auto escape = std::string("\"\\/bfnrt").find(c);
                if (escape == std::string::npos) throw std::runtime_error("json escape");
                c = std::string("\"\\/\b\f\n\r\t")[escape];
            }
            out += c;
        }
    }
    void value(const std::string &key, unsigned depth) {
        if (depth > 32) throw std::runtime_error("json depth");
        space();
        if (at == text.size()) throw std::runtime_error("json empty");
        if (text[at] == '{' || text[at] == '[') {
            const bool object = get() == '{';
            const char end = object ? '}' : ']';
            space();
            if (at < text.size() && text[at] == end) { ++at; return; }
            while (true) {
                space();
                const auto child = object ? string() : std::string{};
                if (object) { space(); if (get() != ':') throw std::runtime_error("json colon"); }
                value(child, depth + 1);
                space();
                const auto separator = get();
                if (separator == end) break;
                if (separator != ',') throw std::runtime_error("json separator");
            }
        } else {
            std::string out;
            if (text[at] == '"') out = string();
            else {
                const auto first = at;
                while (at < text.size() && text[at] != ',' && text[at] != ']' && text[at] != '}' &&
                       !std::isspace(static_cast<unsigned char>(text[at]))) ++at;
                out = text.substr(first, at - first);
                if (out.empty()) throw std::runtime_error("json scalar");
            }
            if (!key.empty()) fields.emplace(key, out);
        }
    }
public:
    std::map<std::string, std::string> fields;
    explicit Fields(const std::string &s) : text(s) { value({}, 0); space(); if (at != text.size()) throw std::runtime_error("json tail"); }
    std::string operator[](const std::string &key) const { auto i = fields.find(key); return i == fields.end() ? "" : i->second; }
};
std::string quote(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c < 32) throw std::runtime_error("invalid API identity");
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}
std::string base64(const unsigned char *bytes, size_t size) {
    std::string result(4 * ((size + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&result[0]), bytes, int(size));
    return result;
}
class Tls {
    int fd = -1;
    SSL_CTX *context = nullptr;
    SSL *ssl = nullptr;
public:
    Tls() = default;
    Tls(const Tls &) = delete;
    ~Tls() { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); if (fd >= 0) close(fd); }
    void connect(const std::string &host) {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error("socket");
        timeval timeout{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(1443);
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
            ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address)) throw std::runtime_error("connect");
        context = SSL_CTX_new(TLS_client_method());
        if (!context) throw std::runtime_error("TLS context");
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
        ssl = SSL_new(context);
        if (!ssl) throw std::runtime_error("TLS allocation");
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) != 1) throw std::runtime_error("TLS handshake");
    }
    void send(const std::string &bytes) {
        size_t at = 0;
        while (at < bytes.size()) {
            int n = SSL_write(ssl, bytes.data() + at, int(bytes.size() - at));
            if (n <= 0) throw std::runtime_error("socket write");
            at += n;
        }
    }
    std::string read(size_t count) {
        std::string out(count, '\0'); size_t at = 0;
        while (at < count) {
            int n = SSL_read(ssl, &out[at], int(count - at));
            if (n <= 0) throw std::runtime_error("socket read");
            at += n;
        }
        return out;
    }
    std::string headers() {
        std::string out;
        while (out.size() < 16384) { out += read(1); if (out.size() >= 4 && out.substr(out.size() - 4) == "\r\n\r\n") return out; }
        throw std::runtime_error("HTTP header too large");
    }
    std::string info(const std::string &host) {
        connect(host);
        send("GET /api/v1/players/local/info HTTP/1.1\r\nHost: " + host + ":1443\r\nX-Sonos-Api-Key: 123e4567-e89b-12d3-a456-426655440000\r\nConnection: close\r\n\r\n");
        auto h = headers();
        if (h.find(" 200 ") == std::string::npos) throw std::runtime_error("local info HTTP status");
        auto at = h.find("Content-Length:");
        if (at == std::string::npos) at = h.find("content-length:");
        if (at == std::string::npos) throw std::runtime_error("local info missing length");
        const auto size = std::stoul(h.substr(at + 15));
        if (size > 65536) throw std::runtime_error("local info too large");
        return read(size);
    }
    void websocket(const std::string &host) {
        connect(host);
        unsigned char nonce[16]; if (RAND_bytes(nonce, sizeof nonce) != 1) throw std::runtime_error("random");
        const auto key = base64(nonce, sizeof nonce);
        send("GET /websocket/api HTTP/1.1\r\nHost: " + host + ":1443\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Protocol: v1.api.smartspeaker.audio\r\nX-Sonos-Api-Key: 123e4567-e89b-12d3-a456-426655440000\r\n\r\n");
        const auto h = headers();
        unsigned char digest[SHA_DIGEST_LENGTH];
        const auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        SHA1(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);
        if (h.find(" 101 ") == std::string::npos || h.find(base64(digest, sizeof digest)) == std::string::npos ||
            h.find("v1.api.smartspeaker.audio") == std::string::npos) throw std::runtime_error("websocket handshake");
    }
    void frame(const std::string &body, unsigned opcode = 1) {
        if (body.size() > 65535) throw std::runtime_error("websocket send size");
        std::string bytes(1, char(0x80 | opcode));
        if (body.size() < 126) bytes += char(0x80 | body.size());
        else { bytes += char(0xfe); bytes += char(body.size() >> 8); bytes += char(body.size()); }
        unsigned char mask[4]; if (RAND_bytes(mask, sizeof mask) != 1) throw std::runtime_error("random");
        bytes.append(reinterpret_cast<char *>(mask), 4);
        for (size_t i = 0; i < body.size(); ++i) bytes += char(body[i] ^ mask[i % 4]);
        send(bytes);
    }
    bool readable() const {
        pollfd p{fd, POLLIN, 0};
        return SSL_pending(ssl) || ::poll(&p, 1, 0) > 0;
    }
    std::string message() {
        std::string message;
        for (;;) {
            auto h = read(2); const auto opcode = h[0] & 15; const bool final = h[0] & 0x80;
            uint64_t size = h[1] & 127;
            if (h[1] & 0x80) throw std::runtime_error("masked server frame");
            if (size == 126) { auto b = read(2); size = (uint8_t(b[0]) << 8) | uint8_t(b[1]); }
            else if (size == 127) { auto b = read(8); size = 0; for (unsigned char c : b) size = (size << 8) | c; }
            if (size > 65536 || message.size() + size > 65536) throw std::runtime_error("websocket message too large");
            const auto body = read(size);
            if (opcode == 8) throw std::runtime_error("socket closed");
            if (opcode == 9) { frame(body, 10); continue; }
            if (opcode == 10) continue;
            if (opcode != 0 && opcode != 1) throw std::runtime_error("websocket opcode");
            message += body;
            if (final) return message;
        }
    }
};
}
struct WebsocketSpeakerPosition::Impl {
    std::string host, error = "connecting";
    mutable std::mutex mutex;
    PositionFit fit;
    std::atomic_bool stop{false}, regroup{false};
    double last = 0;
    std::thread worker;
    explicit Impl(std::string h) : host(std::move(h)), worker([this] { run(); }) {}
    ~Impl() { stop = true; worker.join(); }
    void run() {
        while (!stop) {
            try {
                Tls lookup;
                const Fields info(lookup.info(host));
                const auto group = info["groupId"], household = info["householdId"];
                if (group.empty() || household.empty()) throw std::runtime_error("local info missing identity");
                Tls connection; connection.websocket(host);
                unsigned serial = 0;
                auto command = [&](const std::string &space, const std::string &name) {
                    const auto id = std::to_string(++serial);
                    connection.frame("[{\"namespace\":" + quote(space) + ",\"command\":" + quote(name) +
                        ",\"householdId\":" + quote(household) + ",\"groupId\":" + quote(group) +
                        ",\"cmdId\":" + quote(id) + "},{}]");
                    return id;
                };
                command("playback:1", "subscribe"); command("playbackMetadata:1", "subscribe");
                double next = 0;
                while (!stop && !regroup.exchange(false)) {
                    if (positionMonotonic() < next) {
                        if (connection.readable()) {
                            const Fields event(connection.message());
                            if (event["type"] == "error" || !event["errorCode"].empty()) throw std::runtime_error("command error");
                            if (event["command"] == "playbackStatus" || event["command"] == "metadataStatus") next = 0;
                        } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    const auto sent = positionMonotonic();
                    const auto id = command("playback:1", "getPlaybackStatus");
                    bool immediate = false;
                    for (;;) {
                        if (stop) return;
                        if (positionMonotonic() - sent > 3) throw std::runtime_error("no answer for 3 s");
                        const Fields answer(connection.message());
                        if (answer["type"] == "error" || !answer["errorCode"].empty()) throw std::runtime_error("command error");
                        if (answer["cmdId"] != id) { immediate = true; continue; }
                        const auto received = positionMonotonic();
                        const auto state = answer["playbackState"], pos = answer["positionMillis"];
                        if (state.empty() || pos.empty()) throw std::runtime_error("playback status missing fields");
                        size_t used = 0; const double position = std::stod(pos, &used);
                        if (used != pos.size()) throw std::runtime_error("invalid position");
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (fit.add({answer["itemId"], state, position, (sent + received) / 2, (received - sent) * 1000})) {
                                last = received; error.clear();
                            } else if ((received - sent) * 1000 <= 200) immediate = true;
                        }
                        next = immediate ? received : sent + ((state == "PLAYING" || state == "TRANSITIONING") ? .2 : 1);
                        break;
                    }
                }
            } catch (const std::exception &e) {
                { std::lock_guard<std::mutex> lock(mutex); error = e.what(); fit.reset(); }
                const auto retry = positionMonotonic() + 30;
                while (!stop && !regroup.exchange(false) && positionMonotonic() < retry)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
};
WebsocketSpeakerPosition::WebsocketSpeakerPosition(std::string host) : impl(new Impl(std::move(host))) {}
WebsocketSpeakerPosition::~WebsocketSpeakerPosition() = default;
std::optional<SpeakerPositionSample> WebsocketSpeakerPosition::position(double now) const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return now - impl->last <= 3 && impl->error.empty() ? impl->fit.position(now) : std::nullopt;
}
std::string WebsocketSpeakerPosition::failure() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->error; }
void WebsocketSpeakerPosition::groupChanged() { impl->regroup = true; }
} // namespace bridge
