// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "test_temp.h"
#include "upnp/http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace {
int fixtureSendBuffer = 0;
}

// The production listener normally accepts connections on all interfaces.
extern "C" int __real_bind(int fd, const sockaddr *address, socklen_t length);
extern "C" int __wrap_bind(int fd, const sockaddr *address, socklen_t length) {
    if (length == sizeof(sockaddr_in) && address->sa_family == AF_INET) {
        if (fixtureSendBuffer &&
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &fixtureSendBuffer, sizeof fixtureSendBuffer))
            return -1;
        sockaddr_in local;
        std::memcpy(&local, address, sizeof local);
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return __real_bind(fd, reinterpret_cast<const sockaddr *>(&local), length);
    }
    return __real_bind(fd, address, length);
}

namespace {
struct Descriptor {
    const int fd;
    ~Descriptor() { if (fd >= 0) close(fd); }
};

struct Scratch {
    std::string path = testTempRoot() + "/yeney-smoke-XXXXXX";
    Scratch() {
        if (!mkdtemp(path.data())) throw std::runtime_error(std::strerror(errno));
    }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void require(bool condition, const std::string &reason) {
    if (!condition) throw std::runtime_error(reason);
}

void get(unsigned port, const std::string &url, std::string_view expected,
         const std::string &range = {}, const std::string &contentRange = {}) {
    const std::string label = range.empty() ? "GET" : "range GET";
    Descriptor connection{socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    require(connection.fd >= 0, label + " socket creation failed");
    const timeval timeout{2, 0};
    require(!setsockopt(connection.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) &&
            !setsockopt(connection.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout),
            label + " socket timeout setup failed");
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(!connect(connection.fd, reinterpret_cast<sockaddr *>(&peer), sizeof peer),
            label + " loopback connection failed");
    const std::string request = "GET " + url + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
        (range.empty() ? "" : "Range: " + range + "\r\n") + "\r\n";
    for (size_t sent = 0; sent < request.size();) {
        const auto count = send(connection.fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, label + " request write failed");
        sent += count;
    }
    std::string wire;
    char buffer[8192];
    while (true) {
        const auto count = recv(connection.fd, buffer, sizeof buffer, 0);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, label + " response read failed");
        if (!count) break;
        wire.append(buffer, count);
        require(wire.size() <= expected.size() + 4096, label + " oversized response");
    }
    const auto headerEnd = wire.find("\r\n\r\n");
    require(headerEnd != std::string::npos,
            label + " missing HTTP headers (received " + std::to_string(wire.size()) + " bytes)");
    const std::string_view header(wire.data(), headerEnd + 2);
    const std::string_view body(wire.data() + headerEnd + 4, wire.size() - headerEnd - 4);
    const std::string_view status = range.empty() ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 206 Partial Content\r\n";
    require(header.substr(0, status.size()) == status, label + " unexpected HTTP status");
    require(header.find("\r\nContent-Length: " + std::to_string(expected.size()) + "\r\n") != header.npos,
            label + " incorrect Content-Length");
    if (!range.empty())
        require(header.find("\r\nContent-Range: " + contentRange + "\r\n") != header.npos,
                label + " incorrect Content-Range");
    require(body.size() == expected.size(),
            label + " delivered " + std::to_string(body.size()) + " of " + std::to_string(expected.size()) + " bytes");
    require(body == expected, label + " WAV bytes differ");
}

void check() {
    Scratch scratch;
    queue_audio::Config config;
    config.root = scratch.path;
    queue_audio::Store store("smoke", config);
    queue_audio::Route route("smoke", "smoke-session", queue_audio::Format::Wav);
    upnp::HttpServer server(0);
    route.registerWith(server);

    constexpr unsigned rate = 44100;
    const std::vector<yeney::Frame> pcm(2 * rate, {0x12345600, -0x12345600});
    const auto item = store.create({"smoke-item", rate, pcm.size(), 1});
    item->append(pcm.data(), pcm.size(), false);
    item->finish();
    const auto url = route.publish("smoke", 1, 1, item);

    // PCM WAV, 2 s at 44 100 Hz, stereo, 24 bits per sample.
    constexpr char wavHeader[] =
        "RIFF\x54\x13\x08\0WAVEfmt \x10\0\0\0\x01\0\x02\0"
        "\x44\xac\0\0\x98\x09\x04\0\x06\0\x18\0data\x30\x13\x08\0";
    constexpr char samples[] = "\x56\x34\x12\xaa\xcb\xed";
    std::string expected(wavHeader, sizeof wavHeader - 1);
    for (size_t frame = 0; frame < pcm.size(); ++frame)
        expected.append(samples, sizeof samples - 1);
    get(server.port(), url, expected);

    constexpr size_t first = 45;
    constexpr size_t length = 1024;
    constexpr size_t last = first + length - 1;
    const auto bounds = std::to_string(first) + "-" + std::to_string(last);
    get(server.port(), url, std::string_view(expected).substr(first, length), "bytes=" + bounds,
        "bytes " + bounds + "/" + std::to_string(expected.size()));
}
void pacingFixture() {
    Scratch scratch;
    queue_audio::Config config;
    config.root = scratch.path;
    queue_audio::Store store("pacing", config);
    queue_audio::Route route("pacing", "pacing-session");
    upnp::HttpServer server(0);
    route.registerWith(server);

    constexpr unsigned rate = 44100;
    auto publish = [&](const char *name, unsigned seconds) {
        const auto item = store.create({name, rate, rate * seconds, 1});
        const std::vector<yeney::Frame> pcm(rate, {0x12345600, -0x12345600});
        for (unsigned second = 0; second < seconds; ++second)
            item->append(pcm.data(), pcm.size(), false);
        item->finish();
        const auto url = route.publish("pacing", 1, 1, item);
        return std::make_pair(item, url);
    };
    const auto [item, url] = publish("long", 32);
    const auto [shortItem, shortUrl] = publish("short", 10);
    std::printf("fixture %u %s %s\n", server.port(), url.c_str(), shortUrl.c_str());
    std::fflush(stdout);
    std::string command;
    while (std::cin >> command) {
        if (command == "cancel")
            item->cancel();
        else if (command == "flush")
            item->releaseAudio();
        else
            break;
    }
}

}

int main(int argc, char **argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--pacing-fixture") {
        if (argc == 3 && std::string_view(argv[2]) == "--small-window")
            fixtureSendBuffer = 16384;
        setvbuf(stdout, nullptr, _IOLBF, 0);
        pacingFixture();
        return 0;
    }
    // Production request diagnostics share stdout with the smoke verdict.
    const Descriptor verdict{dup(STDOUT_FILENO)};
    const Descriptor quiet{open("/dev/null", O_WRONLY | O_CLOEXEC)};
    if (verdict.fd < 0 || quiet.fd < 0 || dup2(quiet.fd, STDOUT_FILENO) < 0) {
        std::puts("yeney smoke check: FAIL stdout setup");
        return 1;
    }
    try {
        check();
        dprintf(verdict.fd, "yeney smoke check: pass\n");
        return 0;
    } catch (const std::exception &error) {
        dprintf(verdict.fd, "yeney smoke check: FAIL %s\n", error.what());
        return 1;
    }
}
