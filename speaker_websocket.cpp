// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_websocket.h"
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <sstream>
#include <iostream>
#include <limits>
namespace bridge {
double positionMonotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct ConnectionCounts {
    std::atomic_uint held{0}, peak{0}, opened{0}, websockets{0}, websocketPeak{0};
};
namespace {
// Bounded JSON reader: retain scalar fields while validating the entire message.
class Fields {
    const std::string &text;
    size_t at = 0;
    std::string prefix;
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
                const auto saved = prefix.size();
                if (!child.empty()) {
                    if (!prefix.empty()) prefix += '.';
                    prefix += child;
                }
                value(child, depth + 1);
                prefix.resize(saved);
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
            // Retain only the scoped keys needed for current-item diagnostics.
            if (prefix == "container.id.objectId" || prefix == "currentItem.track.quality.bitDepth" ||
                prefix == "currentItem.track.quality.sampleRate" || prefix == "currentItem.track.quality.lossless")
                fields.emplace(prefix, out);
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
std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
struct HttpHeaders {
    std::map<std::string, std::string> fields;
    unsigned status = 0;
    explicit HttpHeaders(const std::string &raw) {
        const auto end = raw.find("\r\n");
        std::istringstream line(raw.substr(0, end));
        std::string version, code;
        line >> version >> code;
        if ((lower(version) != "http/1.1" && lower(version) != "http/1.0") || code.size() != 3 ||
            code.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("invalid HTTP status line");
        status = unsigned(std::stoul(code));
        size_t at = end + 2;
        while (at < raw.size() - 2) {
            const auto next = raw.find("\r\n", at), colon = raw.find(':', at);
            if (colon == std::string::npos || colon >= next || colon == at)
                throw std::runtime_error("invalid HTTP header");
            const auto name = lower(raw.substr(at, colon - at));
            const auto value = trim(raw.substr(colon + 1, next - colon - 1));
            if (fields.count(name)) fields[name] += "," + value;
            else fields[name] = value;
            at = next + 2;
        }
    }
    std::string get(const std::string &name) const {
        const auto i = fields.find(name);
        return i == fields.end() ? "" : i->second;
    }
    bool token(const std::string &name, const std::string &wanted) const {
        std::istringstream values(get(name));
        std::string value;
        while (std::getline(values, value, ',')) if (lower(trim(value)) == lower(wanted)) return true;
        return false;
    }
};
class Tls {
    int fd = -1;
    SSL_CTX *context = nullptr;
    SSL *ssl = nullptr;
    std::string buffered, fragments, awaitingPong;
    double connected = 0, lastSent = 0, pingSent = 0;
    std::string measuredPing;
    double measuredPingSent = 0;
    std::optional<double> measuredPong;
    std::shared_ptr<ConnectionCounts> counts;
    bool websocketReady = false;
public:
    std::function<void(const std::string &)> observe = [](const std::string &) {};
    explicit Tls(std::shared_ptr<ConnectionCounts> c = {}) : counts(std::move(c)) {}
    Tls(const Tls &) = delete;
    ~Tls() {
        if (ssl) SSL_free(ssl);
        if (context) SSL_CTX_free(context);
        if (fd >= 0) { close(fd); if (counts) --counts->held; }
        if (counts && websocketReady) --counts->websockets;
    }
    void connect(const std::string &endpoint) {
        auto host = endpoint;
        unsigned port = 1443;
        const auto colon = host.find(':');
        if (colon != std::string::npos) {
            size_t consumed = 0;
            port = std::stoul(host.substr(colon + 1), &consumed);
            if (consumed != host.size() - colon - 1) throw std::runtime_error("invalid API port");
            host.resize(colon);
            if (!port || port > 65535) throw std::runtime_error("invalid API port");
        }
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error("socket");
        if (counts) {
            ++counts->opened;
            const auto held = ++counts->held;
            if (held > counts->peak) counts->peak = held;
        }
        timeval timeout{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
            ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address)) throw std::runtime_error("connect");
        context = SSL_CTX_new(TLS_client_method());
        if (!context) throw std::runtime_error("TLS context");
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
        ssl = SSL_new(context);
        if (!ssl) throw std::runtime_error("TLS allocation");
        SSL_set_fd(ssl, fd);
        const std::set<std::string> required{
            "ECDHE-RSA-CHACHA20-POLY1305", "ECDHE-RSA-AES256-GCM-SHA384", "ECDHE-RSA-AES128-GCM-SHA256"};
        std::set<std::string> offered;
        for (int n = 0; const auto cipher = SSL_get_cipher_list(ssl, n); ++n) offered.insert(cipher);
        for (const auto &cipher : required)
            if (!offered.count(cipher)) throw std::runtime_error("required S2 TLS suite unavailable: " + cipher);
        if (SSL_connect(ssl) != 1) throw std::runtime_error("TLS handshake");
        connected = lastSent = positionMonotonic();
        observe("TLS protocol=" + std::string(SSL_get_version(ssl)) + " cipher=" + SSL_get_cipher_name(ssl));
        for (const auto &cipher : required) observe("TLS offered_s2=" + cipher);
    }
    void send(const std::string &bytes) {
        size_t at = 0;
        while (at < bytes.size()) {
            int n = SSL_write(ssl, bytes.data() + at, int(bytes.size() - at));
            if (n <= 0) {
                const auto error = SSL_get_error(ssl, n);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    pollfd p{fd, short(error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 0};
                    if (::poll(&p, 1, 1000) > 0) continue;
                }
                throw std::runtime_error("TLS write failed");
            }
            at += n;
        }
        lastSent = positionMonotonic();
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
        send("GET /api/v1/players/local/info HTTP/1.1\r\nHost: " + (host.find(':') == std::string::npos ? host + ":1443" : host) + "\r\nX-Sonos-Api-Key: 123e4567-e89b-12d3-a456-426655440000\r\nConnection: close\r\n\r\n");
        HttpHeaders h(headers());
        while (h.status >= 100 && h.status < 200) h = HttpHeaders(headers());
        if (h.status != 200) throw std::runtime_error("local info HTTP status");
        const size_t cap = 65536;
        std::string body;
        if (h.fields.count("transfer-encoding")) {
            if (lower(h.get("transfer-encoding")) != "chunked") throw std::runtime_error("local info unsupported transfer encoding");
            auto line = [&] {
                std::string value;
                while (value.size() < 16384) {
                    value += read(1);
                    if (value.size() >= 2 && value.substr(value.size() - 2) == "\r\n") return value.substr(0, value.size() - 2);
                }
                throw std::runtime_error("local info chunk line too large");
            };
            try {
                for (;;) {
                    auto text = line();
                    text = text.substr(0, text.find(';'));
                    if (text.empty() || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                        throw std::runtime_error("local info invalid chunk size");
                    size_t used = 0;
                    unsigned long long size;
                    try { size = std::stoull(text, &used, 16); }
                    catch (...) { throw std::runtime_error("local info too large"); }
                    if (size > cap - body.size()) throw std::runtime_error("local info too large");
                    if (!size) {
                        size_t trailers = 0;
                        for (;;) {
                            auto trailer = line();
                            trailers += trailer.size() + 2;
                            if (trailers > 16384) throw std::runtime_error("local info trailers too large");
                            if (trailer.empty()) return body;
                            if (trailer.find(':') == std::string::npos) throw std::runtime_error("local info invalid trailer");
                        }
                    }
                    body += read(size);
                    if (read(2) != "\r\n") throw std::runtime_error("local info invalid chunk delimiter");
                }
            } catch (const std::runtime_error &e) {
                if (std::string(e.what()) == "socket read") throw std::runtime_error("local info truncated chunk");
                throw;
            }
        }
        if (h.fields.count("content-length")) {
            auto value = h.get("content-length");
            std::istringstream lengths(value);
            std::string length;
            value.clear();
            while (std::getline(lengths, length, ',')) {
                length = trim(length);
                if (length.empty() || length.find_first_not_of("0123456789") != std::string::npos ||
                    (!value.empty() && length != value)) throw std::runtime_error("local info invalid length");
                value = length;
            }
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("local info invalid length");
            unsigned long long size;
            try { size = std::stoull(value); } catch (...) { throw std::runtime_error("local info too large"); }
            if (size > cap) throw std::runtime_error("local info too large");
            try { return read(size); } catch (...) { throw std::runtime_error("local info truncated body"); }
        }
        for (;;) {
            char c;
            const int n = SSL_read(ssl, &c, 1);
            if (n <= 0) {
                const int error = SSL_get_error(ssl, n);
                if (error == SSL_ERROR_ZERO_RETURN || (error == SSL_ERROR_SYSCALL && n == 0) ||
                    (error == SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING)) return body;
                throw std::runtime_error("local info close body read");
            }
            if (body.size() == cap) throw std::runtime_error("local info too large");
            body += c;
        }
    }
    void websocket(const std::string &host) {
        connect(host);
        unsigned char nonce[16]; if (RAND_bytes(nonce, sizeof nonce) != 1) throw std::runtime_error("random");
        const auto key = base64(nonce, sizeof nonce);
        send("GET /websocket/api HTTP/1.1\r\nHost: " + (host.find(':') == std::string::npos ? host + ":1443" : host) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Protocol: v1.api.smartspeaker.audio\r\nX-Sonos-Api-Key: 123e4567-e89b-12d3-a456-426655440000\r\n\r\n");
        const HttpHeaders h(headers());
        unsigned char digest[SHA_DIGEST_LENGTH];
        const auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        SHA1(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);
        if (h.status != 101 || !h.token("upgrade", "websocket") || !h.token("connection", "upgrade") ||
            h.get("sec-websocket-accept") != base64(digest, sizeof digest) ||
            !h.token("sec-websocket-protocol", "v1.api.smartspeaker.audio")) throw std::runtime_error("websocket handshake");
        if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
            throw std::runtime_error("nonblocking WebSocket socket");
        websocketReady = true;
        if (counts) {
            const auto held = ++counts->websockets;
            if (held > counts->websocketPeak) counts->websocketPeak = held;
        }
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
    double age() const { return connected ? positionMonotonic()-connected : 0; }
    void measurePing(double now) {
        measuredPing = "measure-" + std::to_string(now);
        measuredPingSent = now;
        measuredPong.reset();
        frame(measuredPing, 9);
    }
    std::optional<double> pingRtt() const { return measuredPong; }
    void maintenance(double now = positionMonotonic()) {
        if (!awaitingPong.empty() && now - pingSent >= 3)
            throw std::runtime_error("WebSocket pong missing for 3 s");
        if (awaitingPong.empty() && now - lastSent >= 30) {
            awaitingPong = std::to_string(now); pingSent = now;
            frame(awaitingPong, 9);
            observe("websocket control=ping direction=sent age_s=" + std::to_string(now-connected));
        }
    }
    std::optional<std::string> message() {
        for (;;) {
            if (buffered.size() >= 2) {
                const auto first = uint8_t(buffered[0]), second = uint8_t(buffered[1]);
                const auto opcode = first & 15;
                uint64_t size = second & 127;
                const size_t extra = size == 126 ? 2 : size == 127 ? 8 : 0;
                size_t offset = 2 + extra;
                if (second & 128) throw std::runtime_error("masked server frame");
                if (buffered.size() >= offset) {
                    if (extra) {
                        size = 0;
                        for (size_t n = 2; n < offset; ++n) size = (size << 8) | uint8_t(buffered[n]);
                    }
                    if (size > 65536 || fragments.size() + size > 65536)
                        throw std::runtime_error("websocket message too large");
                    if (opcode >= 8 && (!(first & 128) || size > 125))
                        throw std::runtime_error("invalid websocket control frame");
                    if (buffered.size() >= offset + size) {
                        const auto body = buffered.substr(offset, size);
                        buffered.erase(0, offset + size);
                        const auto age = " age_s=" + std::to_string(positionMonotonic()-connected);
                        if (opcode == 8) {
                            const auto code = body.size() >= 2 ? (uint8_t(body[0]) << 8) | uint8_t(body[1]) : 1005;
                            const auto reason = body.size() >= 2 ? body.substr(2) : "";
                            observe("websocket control=close code=" + std::to_string(code) + " reason=" + reason + age);
                            try { frame(body, 8); } catch (...) {}
                            throw std::runtime_error("WebSocket close code=" + std::to_string(code) + " reason=" + reason);
                        }
                        if (opcode == 9) {
                            observe("websocket control=ping direction=received" + age);
                            frame(body, 10);
                            observe("websocket control=pong direction=sent" + age);
                        } else if (opcode == 10) {
                            observe("websocket control=pong direction=received" + age);
                            if (body == awaitingPong) awaitingPong.clear();
                            if (!measuredPing.empty() && body == measuredPing) {
                                measuredPong = (positionMonotonic() - measuredPingSent) * 1000;
                                measuredPing.clear();
                            }
                        } else {
                            if (opcode != 0 && opcode != 1) throw std::runtime_error("websocket opcode");
                            fragments += body;
                            if (first & 128) { auto result = std::move(fragments); fragments.clear(); return result; }
                        }
                        // Control frames do not promise another application frame.
                        // Continue only through buffered bytes or a nonblocking read.
                        continue;
                    }
                }
            }
            char bytes[16384];
            const auto n = SSL_read(ssl, bytes, sizeof bytes);
            if (n > 0) { buffered.append(bytes, n); continue; }
            const auto error = SSL_get_error(ssl, n);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
                (error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) return {};
            const bool end = error == SSL_ERROR_ZERO_RETURN || (error == SSL_ERROR_SYSCALL && n == 0) ||
                (error == SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING);
            const auto reason = end ? "TLS/TCP end" : "TLS read error=" + std::to_string(error);
            observe("websocket transport=end reason=" + reason + " age_s=" + std::to_string(positionMonotonic()-connected));
            throw std::runtime_error(reason);
        }
    }
};
class LocalSession {
    std::string group, household;
    unsigned serial = 0;
    std::map<std::string, std::string> pending;
    std::map<std::string, double> deadlines;
    double renewAt = 0;
    std::string ending = "session finished";
    std::function<void(const std::string &)> report = [](const std::string &) {};
    std::shared_ptr<ConnectionCounts> counts;
public:
    Tls connection;
    explicit LocalSession(std::shared_ptr<ConnectionCounts> c = {}) : counts(c), connection(std::move(c)) {}
    void failPending(const std::string &reason) {
        ending = reason;
        for (const auto &entry : pending)
            report("speaker command failed cmdId=" + entry.first + " command=" + entry.second + " reason=" + reason);
        pending.clear();
        deadlines.clear();
    }
    ~LocalSession() {
        failPending(ending);
        report(std::string(connection.age() ? "websocket connection=end reason=" : "websocket setup=end reason=") + ending + " age_s=" + std::to_string(connection.age()));
    }
    void open(const std::string &host, const std::function<void(const std::string &)> &step) {
        report = step;
        connection.observe = step;
        std::string discovery;
        { Tls lookup(counts); discovery = lookup.info(host); }
        const Fields info(discovery);
        group = info["groupId"]; household = info["householdId"];
        if (group.empty() || household.empty()) throw std::runtime_error("local info missing identity");
        auto endpoint = host;
        const auto url = info["websocketUrl"];
        if (!url.empty()) {
            const std::string prefix = "wss://", suffix = "/websocket/api";
            if (url.rfind(prefix, 0) != 0 || url.size() <= prefix.size() + suffix.size() ||
                url.substr(url.size() - suffix.size()) != suffix)
                throw std::runtime_error("invalid coordinator websocket URL");
            endpoint = url.substr(prefix.size(), url.size() - prefix.size() - suffix.size());
        }
        if (endpoint != host && endpoint != host + ":1443") {
            Tls coordinatorLookup(counts);
            const Fields coordinatorInfo(coordinatorLookup.info(endpoint));
            if (coordinatorInfo["householdId"] != household || coordinatorInfo["groupId"].empty())
                throw std::runtime_error("coordinator local info identity");
            group = coordinatorInfo["groupId"];
        }
        step("local/info OK");
        connection.websocket(endpoint);
        step("WebSocket upgrade OK");
        subscribe();
        const auto deadline = positionMonotonic() + 3;
        while (!pending.empty()) {
            if (positionMonotonic() >= deadline) { failPending("subscription answer missing for 3 s"); throw std::runtime_error("subscription timeout"); }
            message();
        }
    }
    void subscribe() {
        command("playback:1", "subscribe"); command("playbackMetadata:1", "subscribe");
        renewAt = positionMonotonic() + 24 * 60 * 60;
    }
    void maintenance(double now = positionMonotonic()) {
        connection.maintenance();
        for (const auto &entry : deadlines) {
            if (now >= entry.second) {
                const auto reason = "no answer for 3 s command=" + pending.at(entry.first);
                failPending(reason);
                throw std::runtime_error(reason);
            }
        }
        if (now >= renewAt) subscribe();
    }
    std::string command(const std::string &space, const std::string &name) {
        const auto id = std::to_string(++serial);
        connection.frame("[{\"namespace\":" + quote(space) + ",\"command\":" + quote(name) +
            ",\"householdId\":" + quote(household) + ",\"groupId\":" + quote(group) +
            ",\"cmdId\":" + quote(id) + "},{}]");
        pending[id] = space + ":" + name;
        deadlines[id] = positionMonotonic() + 3;
        return id;
    }
    Fields message() {
        maintenance();
        std::optional<std::string> message;
        try { message = connection.message(); }
        catch (const std::exception &e) { failPending(e.what()); throw; }
        if (!message) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return Fields("{}");
        }
        Fields answer(*message);
        const auto type = answer["type"].empty() ? answer["command"] : answer["type"];
        if (type == "groupCoordinatorChanged" || type == "groups" || type == "groupStatus" ||
            (!answer["groupId"].empty() && answer["groupId"] != group)) {
            failPending("group moved; resolving local/info");
            throw std::runtime_error("group moved; resolving local/info");
        }
        if (type == "subscriptionExpired") { subscribe(); return Fields("{}"); }
        if (!answer["cmdId"].empty() && type != "playbackError" &&
            (type == "error" || type == "globalError" || !answer["errorCode"].empty() || answer["success"] == "false")) {
            failPending("command error code=" + answer["errorCode"] + " reason=" + answer["reason"]);
            throw std::runtime_error("command error code=" + answer["errorCode"] + " reason=" + answer["reason"]);
        }
        const auto waiting = pending.find(answer["cmdId"]);
        if (waiting != pending.end()) {
            if (waiting->second == "playback:1:subscribe") report("playback subscription OK");
            if (waiting->second == "playbackMetadata:1:subscribe") report("metadata subscription OK");
            pending.erase(waiting);
            deadlines.erase(answer["cmdId"]);
        }
        return answer;
    }
    static SpeakerPositionSample sample(const Fields &answer, double sent, double received) {
        const auto state = answer["playbackState"], pos = answer["positionMillis"];
        if (state.empty() || pos.empty()) throw std::runtime_error("playback status missing fields");
        size_t used = 0;
        double position;
        try { position = std::stod(pos, &used); }
        catch (...) { throw std::runtime_error("invalid position"); }
        if (used != pos.size() || !std::isfinite(position) || position < 0) throw std::runtime_error("invalid position");
        SpeakerPositionSample result{answer["itemId"], mapSpeakerState(state), position, (sent + received) / 2, (received - sent) * 1000};
        result.rawState = state;
        result.previousItem = answer["previousItemId"];
        const auto previous = answer["previousPositionMillis"];
        if (!previous.empty()) {
            try {
                used = 0; const auto value = std::stod(previous, &used);
                if (used == previous.size() && std::isfinite(value) && value >= 0) result.previousPositionMs = value;
            } catch (...) {} // Optional diagnostic evidence must not invalidate a position.
        }
        return result;
    }
};

}
int positionLinkRateCheck(const std::string &host, double seconds, std::ostream &out) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    int result = 0;
    try {
        LocalSession session(std::make_shared<ConnectionCounts>());
        session.open(host, [](const std::string &) {});
        bool playing = false;
        bool complete = true;
        for (int hz : {5, 10, 20, 40}) {
            for (bool interleaved : {false, true}) {
                std::vector<double> positions, pings;
                const auto measure = [&](double duration, bool position, bool ping) {
                    const double end = positionMonotonic() + duration;
                    while (positionMonotonic() < end) {
                        const auto sent = positionMonotonic();
                        if (ping) session.connection.measurePing(sent);
                        const auto positionSent = positionMonotonic();
                        auto waiting = position ? session.command("playback:1", "getPlaybackStatus") : std::string{};
                        while (!waiting.empty() || (ping && !session.connection.pingRtt() && positionMonotonic() - sent < 1)) {
                            session.maintenance();
                            if (!session.connection.readable()) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                continue;
                            }
                            const auto answer = session.message();
                            if (!waiting.empty() && answer["cmdId"] == waiting) {
                                positions.push_back((positionMonotonic() - positionSent) * 1000);
                                playing |= speakerPlaying(mapSpeakerState(answer["playbackState"]));
                                waiting.clear();
                            }
                        }
                        if (ping && session.connection.pingRtt()) pings.push_back(*session.connection.pingRtt());
                        const double remaining = std::min(end, sent + 1.0 / hz) - positionMonotonic();
                        if (remaining > 0) std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
                    }
                };
                if (interleaved) {
                    measure(seconds / 8, true, true);
                } else {
                    measure(seconds / 16, true, false);
                    measure(seconds / 16, false, true);
                }
                const auto print = [&](const char *name, std::vector<double> &samples) {
                    out << ' ' << name << "_answers=" << samples.size();
                    if (samples.empty()) {
                        complete = false;
                        out << ' ' << name << "_rtt_ms=unavailable";
                        return;
                    }
                    std::sort(samples.begin(), samples.end());
                    const auto count = samples.size();
                    out << ' ' << name << "_median_ms=" << (samples[(count-1)/2] + samples[count/2])/2
                        << ' ' << name << "_p95_ms=" << samples[size_t(std::ceil(count*.95))-1]
                        << ' ' << name << "_max_ms=" << samples.back();
                };
                out << "link check rate_hz=" << hz << " interleaved_pings=" << (interleaved ? "yes" : "no");
                print("position", positions);
                print("ping", pings);
                out << std::endl;
            }
        }
        result = playing && complete ? 0 : 1;
    } catch (const std::exception &e) {
        out << "link check failed reason=" << e.what() << '\n';
        result = 1;
    }
    return result;
}
int positionSourceCheck(const std::string &host, double seconds, std::ostream &out) {
    WebsocketSpeakerPosition source(host, true);
    source.diagnosticRate(5);
    std::set<std::string> states, items;
    std::map<std::string, unsigned> events{{"playbackStatus",0},{"metadataStatus",0},{"playbackError",0}};
    bool playingSeen = false, unknown = false, selected = false;
    const auto traces = [&] {
        for (const auto &line : source.takeJournal()) {
            if (line.rfind("events type=",0)==0) {
                const auto end = line.find(" count=");
                ++events[line.substr(12,end-12)];
            } else out << line << std::endl;
        }
    };
    const auto readyDeadline = positionMonotonic() + 3;
    while (!source.position(positionMonotonic()) && positionMonotonic() < readyDeadline && !source.failureCount()) {
        traces();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    traces();
    if (!source.position(positionMonotonic())) {
        const auto reason = source.failure();
        source.finishDiagnostic(); traces();
        out << (reason.find("local info") != std::string::npos ? "local/info" : "subscriptions")
            << " FAIL reason=" << reason << "\nselected source=upnp\n";
        out << source.connections() << '\n';
        return 1;
    }
    out << source.connections() << '\n';
    for (const int hz : {5, 20}) {
        source.diagnosticRate(hz);
        std::vector<double> rtts;
        struct Segment { std::string item; std::vector<std::pair<double, double>> points; };
        std::vector<Segment> segments;
        bool split = true;
        const auto failures = source.failureCount();
        const auto end = positionMonotonic() + seconds / 2;
        while (positionMonotonic() < end) {
            traces();
            if (!source.takeEvents().empty()) split = true;
            for (const auto &sample : source.takeSamples()) {
                states.insert(sample.rawState); items.insert(sample.item);
                unknown = unknown || sample.state == "UNKNOWN";
                rtts.push_back(sample.rttMs);
                if (!speakerPlaying(sample.state)) { split = true; continue; }
                playingSeen = true;
                bool discontinuity = false;
                if (!segments.empty() && !segments.back().points.empty()) {
                    const auto previous = segments.back().points.back();
                    discontinuity = std::abs(sample.positionMs - previous.second - (sample.sampleTime-previous.first)*1000) > 150;
                }
                if (split || segments.empty() || segments.back().item != sample.item || discontinuity)
                    segments.push_back({sample.item, {}});
                segments.back().points.emplace_back(sample.sampleTime, sample.positionMs);
                split = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        traces();
        std::sort(rtts.begin(), rtts.end());
        const auto metric = [&](double fraction) {
            return rtts.empty() ? std::numeric_limits<double>::quiet_NaN() :
                rtts[std::min(rtts.size()-1, size_t(std::ceil(rtts.size()*fraction))-1)];
        };
        const auto errors = source.failureCount()-failures;
        out << "getPlaybackStatus " << (source.position(positionMonotonic()) ? "OK" : "FAIL")
            << " rate=" << hz << "Hz answers=" << rtts.size() << " errors=" << errors
            << " rtt_ms median=" << (rtts.empty() ? metric(.5) : (rtts[(rtts.size()-1)/2]+rtts[rtts.size()/2])/2)
            << " p95=" << metric(.95) << " max=" << metric(1)
            << " discarded_slow_share=" << (rtts.empty() ? 0 : double(std::count_if(rtts.begin(),rtts.end(),[](double x){return x>200;}))/rtts.size());
        if (segments.empty()) out << " residual_ms=unavailable (speaker not playing)";
        out << std::endl;
        unsigned serial = 0;
        for (const auto &segment : segments) {
            const auto &points = segment.points;
            out << "playing segment=" << ++serial << " itemId=" << segment.item << " rate=" << hz << "Hz samples=" << points.size();
            if (points.size() < 2) { out << " residual_ms=unavailable (fewer than two samples)\n"; continue; }
            double x=0, y=0, xx=0, xy=0;
            const auto origin = points.front().first;
            for (auto point : points) { const auto t=point.first-origin; x+=t; y+=point.second; xx+=t*t; xy+=t*point.second; }
            const double n=points.size(), denominator=n*xx-x*x;
            const double slope=denominator>0 ? (n*xy-x*y)/denominator : 0, intercept=(y-slope*x)/n;
            double sum=0, maximum=0;
            for (auto point : points) { const auto residual=point.second-intercept-slope*(point.first-origin); sum+=residual*residual; maximum=std::max(maximum,std::abs(residual)); }
            out << " playing_line_slope_ms_s=" << slope << " residual_ms rms=" << std::sqrt(sum/n) << " max=" << maximum << std::endl;
        }
    }
    source.finishDiagnostic(); traces();
    for (const auto &sample : source.takeSamples()) {
        states.insert(sample.rawState); items.insert(sample.item);
        unknown = unknown || sample.state == "UNKNOWN";
    }
    selected = bool(source.position(positionMonotonic()));
    out << "selected source=" << (selected ? "websocket" : "upnp") << std::endl;
    for (const auto &raw : states) out << "playbackState raw=" << raw << " mapped=" << mapSpeakerState(raw) << '\n';
    for (const auto &item : items) out << "itemId=" << item << '\n';
    for (const auto &event : events) out << "events type=" << event.first << " count=" << event.second << '\n';
    out << "playing recognised=" << (playingSeen ? "true" : "false") << std::endl;
    out << source.connections() << '\n';
    return selected && !unknown ? 0 : 1;
}
struct WebsocketSpeakerPosition::Impl {
    std::string host, error = "connecting";
    mutable std::mutex mutex;
    PositionFit fit;
    PositionLink link;
    std::optional<double> trustedAnswerAt;
    std::atomic_bool stop{false}, regroup{false};
    std::atomic_uint diagnosticHz{0};
    bool diagnostic = false, reconnecting = true;
    std::shared_ptr<ConnectionCounts> counts = std::make_shared<ConnectionCounts>();
    std::vector<SpeakerPositionSample> observedSamples;
    double last = 0;
    uint64_t failures = 0, revision = 0, diagnostics = 0;
    std::optional<SpeakerPositionSample> known, lastEvent, heldPosition;
    bool missingPosition = false;
    std::set<std::string> unknownStates;
    std::string quality, qualityItem, playbackError;
    std::vector<SpeakerPositionSample> eventSamples;
    std::vector<std::string> journal;
    std::string rawState;
    std::function<void(const std::string &)> journalSink;
    void emit(const std::string &line) {
        if (journalSink) journalSink(line);
        else journal.push_back(line);
    }
    void holdPosition(const std::string &state, double now) {
        if (!missingPosition) {
            heldPosition = fit.position(now);
            if (heldPosition) {
                heldPosition->state = "TRANSITIONING";
                heldPosition->held = true;
            }
            fit.reset();
            missingPosition = true;
            ++revision;
            emit("speaker position unavailable state=" + state);
        }
        if (heldPosition) heldPosition->rawState = state;
        rawState = state;
        last = now;
        reconnecting = false;
        error.clear();
    }
    bool information(const Fields &answer, double now) {
        const auto type = answer["type"].empty() ? answer["command"] : answer["type"];
        std::lock_guard<std::mutex> lock(mutex);
        if (diagnostic && !type.empty() && answer["cmdId"].empty()) emit("events type=" + type + " count=1");
        if (type == "playbackError") {
            playbackError = "errorCode=" + answer["errorCode"] + " reason=" + answer["reason"] +
                " itemId=" + answer["itemId"] + " httpStatus=" + answer["httpStatus"];
            emit("speaker playbackError " + playbackError); ++diagnostics;
            return false;
        }
        if (type == "metadataStatus" && !answer["currentItem.track.quality.bitDepth"].empty()) {
            const auto item = (known ? known->item : std::string{}) + "|" + answer["container.id.objectId"];
            const auto value = "bitDepth=" + answer["currentItem.track.quality.bitDepth"] + " sampleRate=" + answer["currentItem.track.quality.sampleRate"] + " lossless=" + answer["currentItem.track.quality.lossless"];
            if (qualityItem != item || quality != value) ++diagnostics;
            if (qualityItem != item || quality.empty()) emit("speaker quality " + value);
            quality = value; qualityItem = item;
        }
        if (answer["playbackState"].empty()) return false;
        if (answer["positionMillis"].empty()) {
            const bool entered = !missingPosition;
            holdPosition(answer["playbackState"], now);
            return entered;
        }
        auto sample = LocalSession::sample(answer, now, now);
        sample.event = true;
        eventSamples.push_back(sample);
        if (rawState != sample.rawState) ++revision;
        rawState = sample.rawState;
        if (sample.state == "UNKNOWN" && unknownStates.insert(rawState).second)
            emit("speaker unknown playbackState=" + rawState);
        const bool repeated = lastEvent && sample.item == lastEvent->item && sample.rawState == lastEvent->rawState &&
            sample.positionMs == lastEvent->positionMs && sample.previousItem == lastEvent->previousItem &&
            sample.previousPositionMs == lastEvent->previousPositionMs;
        lastEvent = sample;
        if (repeated) return false;
        const bool explicitSeek = (!known || !speakerTransitioning(known->state)) && sample.previousPositionMs &&
            sample.previousItem == sample.item && std::abs(*sample.previousPositionMs-sample.positionMs) > .5;
        if (!explicitSeek && known && sample.item == known->item && sample.state == known->state) {
            const auto expected = known->positionMs + (speakerPlaying(known->state) ? (now-known->sampleTime)*1000 : 0);
            if (std::abs(sample.positionMs-expected) <= 150) return false;
        }
        if (explicitSeek)
            emit("speaker seek itemId=" + sample.item + " from_ms=" + std::to_string(*sample.previousPositionMs) + " to_ms=" + std::to_string(sample.positionMs));
        sample.event = true;
        if (fit.add(sample)) {
            known = sample;
            last = now;
            error.clear();
            heldPosition.reset();
            missingPosition = false;
            ++revision;
        }
        return true;
    }
    std::thread worker;
    explicit Impl(std::string h, bool d, std::function<void(const std::string &)> sink)
        : host(std::move(h)), diagnostic(d), journalSink(std::move(sink)), worker([this] { run(); }) {}
    ~Impl() { stop = true; if (worker.joinable()) worker.join(); }
    void run() {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
        unsigned consecutiveFailures = 0;
        while (!stop) {
            std::optional<LocalSession> activeSession(std::in_place, counts);
            try {
                auto &session = *activeSession;
                { std::lock_guard<std::mutex> lock(mutex); reconnecting = true; }
                session.open(host, [this](const std::string &line) {
                    std::lock_guard<std::mutex> lock(mutex); emit(line);
                });
                auto &connection = session.connection;
                double next = 0;
                while (!stop && !regroup.exchange(false)) {
                    session.maintenance();
                    if (positionMonotonic() < next) {
                        if (connection.readable()) {
                            const auto event = session.message();
                            if (information(event, positionMonotonic())) next = 0;
                        } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    const auto sent = positionMonotonic();
                    const auto id = session.command("playback:1", "getPlaybackStatus");
                    bool immediate = false;
                    for (;;) {
                        if (stop && !diagnostic) {
                            session.failPending("room shutdown");
                            return;
                        }
                        if (positionMonotonic() - sent > 3) {
                            session.failPending("no answer for 3 s");
                            throw std::runtime_error("no answer for 3 s");
                        }
                        const auto answer = session.message();
                        if (answer["cmdId"] != id) { immediate = information(answer, positionMonotonic()) || immediate; continue; }
                        const auto received = positionMonotonic();
                        if (!answer["playbackState"].empty() && answer["positionMillis"].empty()) {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (!known || !known->event || sent >= known->sampleTime)
                                holdPosition(answer["playbackState"], received);
                            consecutiveFailures = 0;
                            next = received + .05;
                            break;
                        }
                        const auto sample = LocalSession::sample(answer, sent, received);
                        const auto &state = sample.state;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (diagnostic) observedSamples.push_back(sample);
                            if (!diagnostic && link.observe(received, sample.rttMs)) {
                                emit(link.isUneven()
                                     ? "speaker position link=uneven reason=recent-delayed-answers hold_off_s=5"
                                     : "speaker position link=even reason=recent-quick-answers hold_off_s=5");
                                ++revision;
                            }
                            const auto limit = diagnostic ? 200.0 : link.trustedLimit();
                            if (!diagnostic && link.isUneven()) fit.retainQuickAnswers(limit);
                            // Only a request sent after an event can confirm it.
                            // An older in-flight answer may describe the prior coordinate.
                            if (known && known->event && sent < known->sampleTime) {
                                next = received;
                                break;
                            }
                            // A valid status reply proves the connection is live
                            // even if its RTT cannot improve the fitted line.
                            last = received;
                            reconnecting = false;
                            consecutiveFailures = 0;
                            error.clear();
                            const bool trust = sample.rttMs <= limit;
                            if (!trust && known && (known->item != sample.item ||
                                (known->state != sample.state && !speakerPlaying(sample.state))))
                                holdPosition(sample.rawState, received);
                            if (rawState != sample.rawState) ++revision;
                            rawState = sample.rawState;
                            if (sample.state == "UNKNOWN" && unknownStates.insert(rawState).second)
                                emit("speaker unknown playbackState=" + rawState);
                            // A command answer confirms or overrides the event clock.
                            if (known && known->event && trust) fit.reset();
                            if (trust && fit.add(sample, link.quickLimit())) {
                                if (known && sample.item != known->item) ++revision;
                                if (missingPosition) ++revision;
                                known = sample;
                                heldPosition.reset();
                                missingPosition = false;
                                trustedAnswerAt = received;
                            } else if (trust) immediate = true;
                        }
                        const auto hz = diagnosticHz.load();
                        next = immediate ? received : sent + (hz ? 1.0 / hz : (speakerPlaying(state) || speakerTransitioning(state)) ? .05 : 1);
                        break;
                    }
                }
                session.failPending(stop ? "room shutdown" : "speaker group changed");
            } catch (const std::exception &e) {
                activeSession->failPending(e.what());
                activeSession.reset();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    error = e.what(); ++failures; reconnecting = true;
                    emit("speaker reconnecting reason=" + error + " fitted_line_hold_s=3");
                }
                // A diagnostic reports a failed initial setup once. A session
                // that was already healthy still exercises normal reconnection.
                if (diagnostic && last == 0) return;
                const auto delay = consecutiveFailures == 0 ? 0u : 1u << std::min(consecutiveFailures - 1, 4u);
                ++consecutiveFailures;
                const auto retry = positionMonotonic() + delay;
                while (!stop && !regroup.exchange(false) && positionMonotonic() < retry)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
};
WebsocketSpeakerPosition::WebsocketSpeakerPosition(std::string host, bool diagnostic,
                                                     std::function<void(const std::string &)> journal)
    : impl(new Impl(std::move(host), diagnostic, std::move(journal))) {}
WebsocketSpeakerPosition::~WebsocketSpeakerPosition() = default;
std::optional<SpeakerPositionSample> WebsocketSpeakerPosition::position(double now) const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (now - impl->last > 3) return {};
    return impl->missingPosition ? impl->heldPosition : impl->fit.position(now);
}
PositionLinkStatus WebsocketSpeakerPosition::linkStatus() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return {impl->link.isUneven(), impl->fit.precisionMs(), impl->trustedAnswerAt};
}
std::string WebsocketSpeakerPosition::failure() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (positionMonotonic() - impl->last <= 3 && (impl->heldPosition || impl->fit.position(positionMonotonic()))) return {};
    if (impl->error.empty() && positionMonotonic() - impl->last > 3) return "position sample older than 3 s";
    return impl->error;
}
uint64_t WebsocketSpeakerPosition::failureCount() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->failures; }
uint64_t WebsocketSpeakerPosition::revision() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->revision; }
uint64_t WebsocketSpeakerPosition::diagnosticRevision() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->diagnostics; }
std::string WebsocketSpeakerPosition::rawState() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->rawState; }
std::string WebsocketSpeakerPosition::quality() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->quality; }
std::string WebsocketSpeakerPosition::playbackError() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->playbackError; }
std::string WebsocketSpeakerPosition::connectionState() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->reconnecting ? "reconnecting" : "local-api-ready"; }
void WebsocketSpeakerPosition::diagnosticRate(unsigned hz) { impl->diagnosticHz = hz; }
void WebsocketSpeakerPosition::finishDiagnostic() { impl->stop = true; if (impl->worker.joinable()) impl->worker.join(); }
std::string WebsocketSpeakerPosition::connections() const {
    const auto &c = *impl->counts;
    return "connections scope=this-check held_tls=" + std::to_string(c.held) + " peak_tls=" + std::to_string(c.peak) +
        " held_websocket=" + std::to_string(c.websockets) + " peak_websocket=" + std::to_string(c.websocketPeak) +
        " opened_tls=" + std::to_string(c.opened) + " external_connections=not-observed";
}
std::vector<SpeakerPositionSample> WebsocketSpeakerPosition::takeSamples() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto samples = std::move(impl->observedSamples); impl->observedSamples.clear(); return samples;
}
std::vector<SpeakerPositionSample> WebsocketSpeakerPosition::takeEvents() {
    std::lock_guard<std::mutex> lock(impl->mutex); auto result = std::move(impl->eventSamples); impl->eventSamples.clear(); return result;
}
std::vector<std::string> WebsocketSpeakerPosition::takeJournal() {
    std::lock_guard<std::mutex> lock(impl->mutex); auto result = std::move(impl->journal); impl->journal.clear(); return result;
}
void WebsocketSpeakerPosition::groupChanged() { impl->regroup = true; }
} // namespace bridge
