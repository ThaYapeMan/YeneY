// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
namespace queue_audio {
// Diagnostic-only client. One connection and one deadline cover all three
// queries; the caller runs this outside the audio clock and core event loop.
inline std::vector<std::string> pollLms(const std::string &host, unsigned port = 9090) {
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses))
        return {};
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(addresses);
        return {};
    }
    connect(fd, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    const std::string mac = "02:59:51:54:00:01";
    const std::vector<std::string> keys = {"playlist index", "time", "title"};
    std::string request;
    for (const auto &key : keys)
        request += mac + " " + key + " ?\n";
    size_t sent = 0;
    std::string response;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd p{fd, short(sent < request.size() ? POLLOUT : POLLIN), 0};
        if (poll(&p, 1, 20) <= 0)
            continue;
        if (sent < request.size()) {
            auto n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
            if (n <= 0)
                break;
            sent += n;
        } else {
            char bytes[4096];
            auto n = recv(fd, bytes, sizeof(bytes), 0);
            if (n <= 0)
                break;
            response.append(bytes, n);
            if (response.size() > 65536)
                break;
            if (std::count(response.begin(), response.end(), '\n') >= 3)
                break;
        }
    }
    close(fd);
    std::vector<std::string> values;
    size_t at = 0;
    for (const auto &key : keys) {
        auto end = response.find('\n', at);
        const auto prefix = mac + " " + key + " ";
        if (end == std::string::npos || response.compare(at, prefix.size(), prefix))
            return {};
        auto value = response.substr(at + prefix.size(), end - at - prefix.size());
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        values.push_back(value);
        at = end + 1;
    }
    return values;
}
} // namespace queue_audio
