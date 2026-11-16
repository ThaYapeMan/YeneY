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
public:
    Tls() = default;
    Tls(const Tls &) = delete;
    ~Tls() { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); if (fd >= 0) close(fd); }
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
class LocalSession {
    std::string group, household;
    unsigned serial = 0;
public:
    Tls connection;
    void open(const std::string &host, const std::function<void(const std::string &)> &step) {
        Tls lookup;
        const Fields info(lookup.info(host));
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
            Tls coordinatorLookup;
            const Fields coordinatorInfo(coordinatorLookup.info(endpoint));
            if (coordinatorInfo["householdId"] != household || coordinatorInfo["groupId"].empty())
                throw std::runtime_error("coordinator local info identity");
            group = coordinatorInfo["groupId"];
        }
        step("local/info OK");
        connection.websocket(endpoint);
        step("WebSocket upgrade OK");
        command("playback:1", "subscribe"); command("playbackMetadata:1", "subscribe");
    }
    std::string command(const std::string &space, const std::string &name) {
        const auto id = std::to_string(++serial);
        connection.frame("[{\"namespace\":" + quote(space) + ",\"command\":" + quote(name) +
            ",\"householdId\":" + quote(household) + ",\"groupId\":" + quote(group) +
            ",\"cmdId\":" + quote(id) + "},{}]");
        return id;
    }
    Fields message() {
        Fields answer(connection.message());
        if (answer["type"] != "playbackError" && (answer["type"] == "error" || answer["type"] == "globalError" || !answer["errorCode"].empty()))
            throw std::runtime_error("command error code=" + answer["errorCode"] + " reason=" + answer["reason"]);
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
int positionSourceCheck(const std::string &host, double seconds, std::ostream &out) {
    sigset_t blocked, previous;
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    struct Restore { sigset_t mask; ~Restore() { pthread_sigmask(SIG_SETMASK, &mask, nullptr); } } restore{previous};
    std::string step = "local/info";
    try {
        LocalSession session;
        session.open(host, [&](const std::string &message) {
            out << message << std::endl;
            step = message == "local/info OK" ? "WebSocket upgrade" : "subscriptions";
        });
        bool playback = false, metadata = false, selected = false;
        double lastAccepted = 0;
        std::map<std::string, unsigned> events{{"playbackStatus", 0}, {"metadataStatus", 0}, {"playbackError", 0}};
        std::set<std::string> states, items;
        bool playingSeen = false, unknown = false;
        auto receive = [&] {
            auto answer = session.message();
            if (answer["cmdId"].empty()) ++events[answer["type"].empty() ? answer["command"] : answer["type"]];
            if (!answer["itemId"].empty()) items.insert(answer["itemId"]);
            if (!answer["playbackState"].empty()) {
                const auto raw = answer["playbackState"], mapped = mapSpeakerState(raw);
                states.insert(raw); items.insert(answer["itemId"]);
                playingSeen = playingSeen || speakerPlaying(mapped);
                unknown = unknown || mapped == "UNKNOWN";
            }
            if (answer["cmdId"] == "1") { playback = true; out << "playback subscription OK\n"; }
            if (answer["cmdId"] == "2") { metadata = true; out << "metadata subscription OK\n"; }
            return answer;
        };
        const auto subscriptionDeadline = positionMonotonic() + 3;
        while (!playback || !metadata) {
            if (positionMonotonic() > subscriptionDeadline) throw std::runtime_error("subscription timeout");
            receive();
        }
        step = "getPlaybackStatus";
        PositionFit fit;
        for (const int hz : {5, 20}) {
            std::vector<double> rtts;
            std::vector<std::pair<double, double>> points;
            unsigned errors = 0, slow = 0;
            const double end = positionMonotonic() + seconds / 2;
            std::exception_ptr failure;
            try {
                while (positionMonotonic() < end) {
                    const double sent = positionMonotonic();
                    const auto id = session.command("playback:1", "getPlaybackStatus");
                    for (;;) {
                        if (positionMonotonic() - sent > 3) throw std::runtime_error("no answer for 3 s");
                        const auto answer = receive();
                        if (answer["cmdId"] != id) continue;
                        const auto sample = LocalSession::sample(answer, sent, positionMonotonic());
                        rtts.push_back(sample.rttMs);
                        if (sample.rttMs > 200) ++slow;
                        if (fit.add(sample)) { selected = true; lastAccepted = positionMonotonic(); }
                        if (speakerPlaying(sample.state)) points.emplace_back(sample.sampleTime, sample.positionMs);
                        break;
                    }
                    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(sent + 1.0 / hz))));
                }
            } catch (...) { ++errors; failure = std::current_exception(); }
            const auto metric = [&](double fraction) { return rtts.empty() ? std::numeric_limits<double>::quiet_NaN() :
                rtts[std::min(rtts.size()-1, size_t(std::ceil(rtts.size()*fraction))-1)]; };
            std::sort(rtts.begin(), rtts.end());
            out << "getPlaybackStatus " << (failure ? "FAIL" : "OK") << " rate=" << hz << "Hz answers=" << rtts.size() << " errors=" << errors
                << " rtt_ms median=" << (rtts.empty() ? metric(.5) : (rtts[(rtts.size()-1)/2] + rtts[rtts.size()/2])/2) << " p95=" << metric(.95)
                << " max=" << metric(1) << " discarded_slow_share=" << (rtts.empty() ? 0 : double(slow)/rtts.size());
            if (points.size() >= 2) {
                double x = 0, y = 0, xx = 0, xy = 0;
                const double origin = points.front().first;
                for (auto p : points) { const auto t = p.first-origin; x += t; y += p.second; xx += t*t; xy += t*p.second; }
                const double n = points.size(), denominator = n*xx-x*x;
                const double slope = denominator > 0 ? (n*xy-x*y)/denominator : 0, intercept = (y-slope*x)/n;
                double sum = 0, maximum = 0;
                for (auto p : points) { double residual = p.second-intercept-slope*(p.first-origin); sum += residual*residual; maximum = std::max(maximum, std::abs(residual)); }
                out << " playing_line_slope_ms_s=" << slope << " residual_ms rms=" << std::sqrt(sum/n) << " max=" << maximum;
            } else out << " residual_ms=unavailable (speaker not playing)";
            out << std::endl;
            if (failure) std::rethrow_exception(failure);
        }
        selected = selected && positionMonotonic() - lastAccepted <= 3;
        out << "selected source=" << (selected ? "websocket" : "upnp") << std::endl;
        for (const auto &raw : states) out << "playbackState raw=" << raw << " mapped=" << mapSpeakerState(raw) << '\n';
        for (const auto &entry : events) out << "events type=" << entry.first << " count=" << entry.second << '\n';
        for (const auto &item : items) out << "itemId=" << item << '\n';
        out << "playing recognised=" << (playingSeen ? "true" : "false") << std::endl;
        return selected && !unknown ? 0 : 1;
    } catch (const std::exception &e) {
        out << step << " FAIL reason=" << e.what() << "\nselected source=upnp\n";
        return 1;
    }
}
struct WebsocketSpeakerPosition::Impl {
    std::string host, error = "connecting";
    mutable std::mutex mutex;
    PositionFit fit;
    std::atomic_bool stop{false}, regroup{false};
    double last = 0;
    uint64_t failures = 0, revision = 0;
    std::optional<SpeakerPositionSample> known;
    std::set<std::string> unknownStates;
    std::string quality, qualityItem, playbackError;
    std::vector<SpeakerPositionSample> eventSamples;
    std::vector<std::string> journal;
    std::string rawState;
    bool information(const Fields &answer, double now) {
        const auto type = answer["type"].empty() ? answer["command"] : answer["type"];
        std::lock_guard<std::mutex> lock(mutex);
        if (type == "playbackError") {
            playbackError = "errorCode=" + answer["errorCode"] + " reason=" + answer["reason"] +
                " itemId=" + answer["itemId"] + " httpStatus=" + answer["httpStatus"];
            journal.push_back("speaker playbackError " + playbackError); ++revision;
            return false;
        }
        if (type == "metadataStatus" && !answer["bitDepth"].empty()) {
            const auto item = (known ? known->item : std::string{}) + "|" + answer["objectId"];
            const auto value = "bitDepth=" + answer["bitDepth"] + " sampleRate=" + answer["sampleRate"] + " lossless=" + answer["lossless"];
            if (qualityItem != item || quality.empty()) { journal.push_back("speaker quality " + value); ++revision; }
            quality = value; qualityItem = item;
        }
        if (answer["playbackState"].empty()) return false;
        auto sample = LocalSession::sample(answer, now, now);
        sample.event = true;
        eventSamples.push_back(sample);
        if (rawState != sample.rawState) ++revision;
        rawState = sample.rawState;
        if (sample.state == "UNKNOWN" && unknownStates.insert(rawState).second)
            journal.push_back("speaker unknown playbackState=" + rawState);
        if (known && sample.item == known->item && sample.state == known->state) {
            const auto expected = known->positionMs + (speakerPlaying(known->state) ? (now-known->sampleTime)*1000 : 0);
            if (std::abs(sample.positionMs-expected) <= 150) return false;
        }
        if ((!known || !speakerTransitioning(known->state)) && sample.previousPositionMs && sample.previousItem == sample.item &&
            std::abs(*sample.previousPositionMs-sample.positionMs) > 150)
            journal.push_back("speaker seek itemId=" + sample.item + " from_ms=" + std::to_string(*sample.previousPositionMs) + " to_ms=" + std::to_string(sample.positionMs));
        sample.event = true;
        fit.add(sample); known = sample; last = now; error.clear(); ++revision;
        return true;
    }
    std::thread worker;
    explicit Impl(std::string h) : host(std::move(h)), worker([this] { run(); }) {}
    ~Impl() { stop = true; worker.join(); }
    void run() {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
        while (!stop) {
            try {
                LocalSession session;
                session.open(host, [](const std::string &) {});
                auto &connection = session.connection;
                double next = 0;
                while (!stop && !regroup.exchange(false)) {
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
                        if (stop) return;
                        if (positionMonotonic() - sent > 3) throw std::runtime_error("no answer for 3 s");
                        const auto answer = session.message();
                        if (answer["cmdId"] != id) { immediate = information(answer, positionMonotonic()) || immediate; continue; }
                        const auto received = positionMonotonic();
                        const auto sample = LocalSession::sample(answer, sent, received);
                        const auto &state = sample.state;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (rawState != sample.rawState) ++revision;
                            rawState = sample.rawState;
                            if (sample.state == "UNKNOWN" && unknownStates.insert(rawState).second)
                                journal.push_back("speaker unknown playbackState=" + rawState);
                            // A command answer confirms or overrides the event clock.
                            if (known && known->event && sample.rttMs <= 200) fit.reset();
                            if (fit.add(sample)) {
                                known = sample;
                                last = received; error.clear();
                            } else if ((received - sent) * 1000 <= 200) immediate = true;
                        }
                        next = immediate ? received : sent + ((speakerPlaying(state) || speakerTransitioning(state)) ? .05 : 1);
                        break;
                    }
                }
            } catch (const std::exception &e) {
                { std::lock_guard<std::mutex> lock(mutex); error = e.what(); ++failures; fit.reset(); }
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
std::string WebsocketSpeakerPosition::failure() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->error.empty() && positionMonotonic() - impl->last > 3) return "position sample older than 3 s";
    return impl->error;
}
uint64_t WebsocketSpeakerPosition::failureCount() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->failures; }
uint64_t WebsocketSpeakerPosition::revision() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->revision; }
std::string WebsocketSpeakerPosition::rawState() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->rawState; }
std::string WebsocketSpeakerPosition::quality() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->quality; }
std::string WebsocketSpeakerPosition::playbackError() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->playbackError; }
std::vector<SpeakerPositionSample> WebsocketSpeakerPosition::takeEvents() {
    std::lock_guard<std::mutex> lock(impl->mutex); auto result = std::move(impl->eventSamples); impl->eventSamples.clear(); return result;
}
std::vector<std::string> WebsocketSpeakerPosition::takeJournal() {
    std::lock_guard<std::mutex> lock(impl->mutex); auto result = std::move(impl->journal); impl->journal.clear(); return result;
}
void WebsocketSpeakerPosition::groupChanged() { impl->regroup = true; }
} // namespace bridge
