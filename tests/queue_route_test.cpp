// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "upnp/http_server.h"
#include <arpa/inet.h>
#include <cassert>
#include <future>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
using namespace queue_audio;
int main() {
    char temp[] = "/tmp/yeney-route-XXXXXX";
    assert(mkdtemp(temp));
    {
        Config config;
        config.root = temp;
        Store store("Study", config);
        Route route("Study", "random-session");
        upnp::HttpServer server(0);
        route.registerWith(server);
        assert(server.resource("queue").uri == "/music/queue.wav");
        std::vector<yeney::Frame> pcm(44100, {0x12345600, -0x12345600});
        auto item = store.create({"occurrence-1", 44100, pcm.size(), 1});
        item->append(pcm.data(), pcm.size(), false);
        item->finish();
        auto url = route.publish("Study", 2, 3, item);
        auto get = [&](std::string target, std::string method = "GET", std::string range = "") {
            std::string bytes;
            upnp::HttpRequestIO io;
            io.send = [&](const char *p, size_t n) {
                bytes.append(p, n);
                return true;
            };
            io.peerClosed = [] { return false; };
            io.aborted = [] { return false; };
            io.disconnect = [] {};
            io.sendTimeout = [](unsigned) {};
            auto r =
                upnp::httpRequestFromHeaders(method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n" +
                                                 (range.empty() ? "" : "Range: " + range + "\r\n") + "\r\n",
                                             io);
            assert(r);
            assert(route.serve(*r));
            return bytes;
        };
        auto full = get(url);
        auto body = full.substr(full.find("\r\n\r\n") + 4);
        assert(body.size() == 44 + pcm.size() * 6);
        assert(get(url, "HEAD").find("\r\n\r\n") == get(url, "HEAD").size() - 4);
        auto partial = get(url, "GET", "bytes=45-103");
        assert(partial.find("206 Partial Content") != std::string::npos);
        assert(partial.find("Content-Range: bytes 45-103/264644") != std::string::npos);
        assert(partial.substr(partial.find("\r\n\r\n") + 4) == body.substr(45, 59));
        auto suffix = get(url, "GET", "bytes=-17");
        assert(suffix.substr(suffix.find("\r\n\r\n") + 4) == body.substr(body.size() - 17));
        assert(get(url, "GET", "bytes=999999-").find("416") != std::string::npos);
        assert(get(url, "POST").find("405") != std::string::npos);
        auto a = std::async(std::launch::async, [&] { return get(url); });
        auto b = std::async(std::launch::async, [&] { return get(url, "GET", "bytes=100-"); });
        assert(a.get() == full);
        auto tail = b.get();
        assert(tail.substr(tail.find("\r\n\r\n") + 4) == body.substr(100));
        assert(get("/music/queue.wav?session=other&epoch=2&item=occurrence-1&revision=3").find("404") !=
               std::string::npos);
        bool refused = false;
        try {
            route.publish("Kitchen", 2, 3, item);
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused);
        route.revoke(2);
        assert(get(url).find("404") != std::string::npos);
        std::string retained;
        {
            Route temporary("Study", "retained-session");
            temporary.registerWith(server);
            retained = temporary.publish("Study", 4, 1, item);
        }
        auto request = [&](const std::string &method, const std::string &range) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            assert(fd >= 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(server.port());
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            assert(!connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
            timeval timeout{3, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            const auto wire =
                method + " " + retained + " HTTP/1.1\r\nHost: localhost\r\nRange: " + range + "\r\n\r\n";
            assert(send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) == ssize_t(wire.size()));
            std::string response;
            char bytes[8192];
            ssize_t n;
            while ((n = recv(fd, bytes, sizeof(bytes), 0)) > 0)
                response.append(bytes, n);
            assert(n == 0);
            close(fd);
            return response;
        };
        auto network = request("GET", "bytes=45-103");
        assert(network.substr(network.find("\r\n\r\n") + 4) == body.substr(45, 59));
        auto head = request("HEAD", "bytes=-17");
        assert(head.find("206 Partial Content") != std::string::npos &&
               head.size() == head.find("\r\n\r\n") + 4);
        server.shutdown();
    }
    std::filesystem::remove_all(temp);
    std::cout << "PASS queue finite route: room/session/epoch/revision ownership, HEAD/200/206/suffix/416, "
                 "independent reconnect ranges and revocation\n";
}
