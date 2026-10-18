// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_lms_cli.h"
#include "third_party/yeney-core/core/flac_header.h"
#include <cctype>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
namespace queue_audio {
// Metadata only. LMS IPv4 is resolved once before the player connects.
// A single absolute deadline covers CLI identity and HTTP metadata together.
class FlacMetadata {
    using Clock = std::chrono::steady_clock;
    std::string host;
    unsigned cliPort, httpPort;
    in_addr target{};
    bool haveAddress = false;
    std::function<void(const std::string &)> log;
    std::mutex mutex;
    std::map<std::string, yeney::FlacSourceInfo> cache;
    std::optional<uint64_t> baseIndex;
    uint64_t generation = 0;
    std::string exchange(unsigned port, const std::string &request, Clock::time_point deadline, bool http) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (!haveAddress)
            return {};
        address.sin_addr = target;
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return {};
        connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        std::string response;
        size_t sent = 0;
        while (Clock::now() < deadline && response.size() < 16384) {
            pollfd p{fd, short(sent < request.size() ? POLLOUT : POLLIN), 0};
            auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (poll(&p, 1, std::max<int64_t>(0, std::min<int64_t>(20, left))) <= 0)
                continue;
            if (sent < request.size()) {
                auto n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
                if (n <= 0)
                    break;
                sent += n;
            } else {
                char bytes[1024];
                auto n = recv(fd, bytes, sizeof(bytes), 0);
                if (n <= 0)
                    break;
                response.append(bytes, n);
                if (!http && response.find('\n') != std::string::npos)
                    break;
                auto end = response.find("\r\n\r\n");
                // STREAMINFO is the first mandatory block: 4+4+34 bytes.
                if (http && end != std::string::npos && response.size() >= end + 4 + 42)
                    break;
            }
        }
        close(fd);
        return response;
    }
    std::string query(const std::string &key, Clock::time_point deadline) {
        const std::string prefix = "02:59:51:54:00:01 " + key + " ";
        auto response = decodeLms(exchange(cliPort, prefix + "?\n", deadline, false));
        if (response.compare(0, prefix.size(), prefix))
            return {};
        response = response.substr(prefix.size());
        auto end = response.find_first_of("\r\n");
        return response.substr(0, end);
    }
    static bool digits(const std::string &s) {
        return !s.empty() && s.size() <= 18 && s.find_first_not_of("0123456789") == std::string::npos;
    }

public:
    FlacMetadata(std::string server, std::function<void(const std::string &)> logger, unsigned cli = 9090,
                 unsigned http = 9000)
        : host(std::move(server)), cliPort(cli), httpPort(http), log(std::move(logger)) {
        addrinfo hints{}, *addresses = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (!getaddrinfo(host.c_str(), nullptr, &hints, &addresses)) {
            target = reinterpret_cast<sockaddr_in *>(addresses->ai_addr)->sin_addr;
            haveAddress = true;
            freeaddrinfo(addresses);
        }
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        baseIndex.reset();
        ++generation;
    }
    std::optional<yeney::FlacSourceInfo> resolve(uint64_t index, std::optional<yeney::FlacSourceInfo> info,
                                                 std::optional<uint64_t> audibleIndex = std::nullopt) {
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        auto fail = [&](const std::string &why) {
            log(std::string(info ? "FLAC metadata cache unavailable: " : "FLAC metadata sealed fallback: ") +
                why);
            return info; // A stream's own validated STREAMINFO never needs a lookup.
        };
        uint64_t gen;
        std::optional<uint64_t> base;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gen = generation;
            base = index ? baseIndex : std::nullopt;
        }
        if (!base) {
            // On reconnect/seek the first occurrence may be any playlist entry.
            // Subsequent strm occurrences follow it, ahead of the audible index.
            auto current = query("playlist index", deadline);
            if (!digits(current))
                return fail("CLI playlist index unavailable");
            const auto currentIndex = std::stoull(current);
            if (!index)
                base = currentIndex;
            else if (audibleIndex && currentIndex >= *audibleIndex)
                base = currentIndex - *audibleIndex;
            else
                return fail("playlist base unavailable");
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != gen)
                return fail("flushed during identity lookup");
            baseIndex = base;
        }
        const std::string key = "status " + std::to_string(*base + index) + " 1 tags:";
        // status is an extended query, not a '?' query.
        const std::string prefix = "02:59:51:54:00:01 " + key;
        auto response = decodeLms(exchange(cliPort, prefix + "\n", deadline, false));
        if (response.compare(0, prefix.size(), prefix))
            return fail("CLI track id unavailable");
        std::istringstream tokens(response.substr(prefix.size()));
        std::string token, id;
        bool entry = false;
        while (tokens >> token) {
            if (token == "playlist_index:" + std::to_string(*base + index))
                entry = true;
            if (entry && token.rfind("id:", 0) == 0) {
                id = token.substr(3);
                break;
            }
        }
        if (!digits(id))
            return fail("CLI track id unavailable");
        if (Clock::now() >= deadline)
            return fail("identity deadline");
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != gen)
                return fail("flushed during identity lookup");
            if (info) {
                cache[id] = *info;
                log("FLAC STREAMINFO cached track_id=" + id);
                return info;
            }
            auto found = cache.find(id);
            if (found != cache.end()) {
                log("FLAC metadata cache track_id=" + id);
                return found->second;
            }
        }
        auto raw =
            exchange(httpPort,
                     "GET /music/" + id + "/download HTTP/1.1\r\nHost: " + host + ":" +
                         std::to_string(httpPort) + "\r\nRange: bytes=0-8191\r\nConnection: close\r\n\r\n",
                     deadline, true);
        if (Clock::now() >= deadline)
            return fail("metadata deadline");
        auto end = raw.find("\r\n\r\n");
        if (end == std::string::npos || end > 8192 || raw.size() < end + 46)
            return fail("HTTP STREAMINFO unavailable");
        auto headers = raw.substr(0, end);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if ((headers.rfind("http/1.1 200 ", 0) && headers.rfind("http/1.0 200 ", 0) &&
             headers.rfind("http/1.1 206 ", 0) && headers.rfind("http/1.0 206 ", 0)) ||
            headers.find("transfer-encoding:") != std::string::npos ||
            headers.find("content-encoding:") != std::string::npos)
            return fail("unsupported metadata HTTP response");
        if (headers.substr(9, 3) == "206" && headers.find("content-range: bytes 0-") == std::string::npos)
            return fail("metadata range does not start at zero");
        auto p = reinterpret_cast<const uint8_t *>(raw.data() + end + 4);
        if (std::string(reinterpret_cast<const char *>(p), 4) != "fLaC" || (p[4] & 127) || p[5] || p[6] ||
            p[7] != 34)
            return fail("missing STREAMINFO");
        yeney::FlacSourceInfo fetched;
        fetched.fixedBlockSize = unsigned(p[10]) * 256 + p[11];
        fetched.totalSamples = p[21] & 15;
        for (unsigned i = 22; i < 26; ++i)
            fetched.totalSamples = (fetched.totalSamples << 8) | p[i];
        if (!fetched.totalSamples || fetched.fixedBlockSize < 16)
            return fail("unknown STREAMINFO count");
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != gen)
                return fail("flushed during metadata lookup");
            cache[id] = fetched;
        }
        log("FLAC metadata HTTP range track_id=" + id +
            " total_samples=" + std::to_string(fetched.totalSamples));
        return fetched;
    }
};
} // namespace queue_audio
