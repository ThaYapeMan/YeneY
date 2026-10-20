// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
namespace queue_audio {
inline std::string decodeLms(const std::string &input) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size() && hex(input[i + 1]) >= 0 && hex(input[i + 2]) >= 0) {
            out += char(hex(input[i + 1]) * 16 + hex(input[i + 2]));
            i += 2;
        } else
            out += input[i];
    }
    return out;
}
// Queue control adapter. Raw MAC on the wire, decoded exact acknowledgement,
// one bounded attempt. Bind this to ControllerOptions::lmsControl at room
// start.
inline bool controlLms(const std::string &host, const std::string &mac, const std::string &command,
                       unsigned port = 9090, std::string *reply = nullptr, unsigned timeoutMs = 0,
                       const std::atomic_bool *cancel = nullptr) {
    if (mac.size() != 17 || command.find_first_of("\r\n") != std::string::npos)
        return false;
    for (size_t i = 0; i < mac.size(); ++i)
        if (i % 3 == 2 ? mac[i] != ':' : !std::isxdigit(static_cast<unsigned char>(mac[i])))
            return false;
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses))
        return false;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(addresses);
        return false;
    }
    connect(fd, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(
            timeoutMs ? timeoutMs
                      : (command.rfind("status ", 0) == 0 || command.find("?") == std::string::npos ? 30000
                                                                                                    : 3000));
    const auto request = mac + " " + command + "\n";
    std::string response;
    size_t sent = 0;
    while (std::chrono::steady_clock::now() < deadline && (!cancel || !*cancel)) {
        pollfd p{fd, short(sent < request.size() ? POLLOUT : POLLIN), 0};
        if (poll(&p, 1, 20) <= 0)
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
            if (response.find('\n') != std::string::npos ||
                response.size() > (reply ? 16 * 1024 * 1024 : 4096))
                break;
        }
    }
    close(fd);
    auto end = response.find('\n');
    if (end == std::string::npos)
        return false;
    // Split before decoding: encoded spaces belong to a single argument.
    std::istringstream expectedInput(mac + " " + command), actualInput(response.substr(0, end));
    std::vector<std::string> expected, actual;
    std::string word;
    while (expectedInput >> word)
        expected.push_back(decodeLms(word));
    while (actualInput >> word)
        actual.push_back(decodeLms(word));
    const bool question = !expected.empty() && expected.back() == "?";
    if (question)
        expected.pop_back();
    const bool query = question || command.rfind("status ", 0) == 0;
    // Some clients encode the complete acknowledgement, including separators.
    // Accept only an exact decoded mutation echo in that representation.
    if (!query && actual.size() == 1 && actual.front() == decodeLms(mac + " " + command))
        actual = expected;
    if (actual.size() < expected.size() || !std::equal(expected.begin(), expected.end(), actual.begin()))
        return false;
    if (!query && actual.size() != expected.size())
        return false;
    for (size_t i = expected.size(); i < actual.size(); ++i)
        if (actual[i].rfind("error:", 0) == 0 || actual[i].rfind("badparams:", 0) == 0)
            return false;
    if (reply)
        *reply = response.substr(0, end);
    return !question || actual.size() > expected.size();
}
// Diagnostic-only client. One connection and one deadline cover all three
// queries; the caller runs this outside the audio clock and core event loop.
inline std::vector<std::string> pollLms(const std::string &host, unsigned port = 9090,
                                        std::string *raw = nullptr) {
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
    if (raw)
        *raw = response.substr(0, response.find('\n'));
    std::vector<std::string> values;
    size_t at = 0;
    for (const auto &key : keys) {
        auto end = response.find('\n', at);
        const auto line = decodeLms(response.substr(at, end == std::string::npos ? end : end - at));
        const auto prefix = mac + " " + key + " ";
        if (end == std::string::npos || line.compare(0, prefix.size(), prefix))
            return {};
        auto value = line.substr(prefix.size());
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        values.push_back(value);
        at = end + 1;
    }
    return values;
}
} // namespace queue_audio
