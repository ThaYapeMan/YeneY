#include "test_temp.h"
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_route.h"
#include "queue_lazy_item.h"
#include "upnp/http_server.h"
#include <arpa/inet.h>
#include <cassert>
#include <future>
#include <atomic>
#include <thread>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
using namespace queue_audio;
int main() {
    std::string temp = testTempRoot()+"/yeney-route-XXXXXX";
    assert(mkdtemp(temp.data()));
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
        auto declaration = store.create({"lazy-1", 44100, pcm.size(), 1});
        unsigned starts = 0;
        bool abandoned = false;
        auto lazyUrl = route.publish("Study", 2, 3, declaration,
            [&](upnp::StreamRequest &request) -> std::shared_ptr<Item> {
                ++starts;
                if (abandoned) return {};
                assert(!request.peerClosed());
                return item;
            });
        assert(get(lazyUrl, "HEAD").find("Content-Length: 264644") != std::string::npos);
        assert(starts == 0);
        assert(get(lazyUrl, "GET", "bytes=999999-").find("416") != std::string::npos);
        assert(starts == 0);
        auto headerProbe = get(lazyUrl, "GET", "bytes=0-43");
        assert(headerProbe.size() == headerProbe.find("\r\n\r\n") + 4 + 44);
        assert(starts == 0);
        auto lazyFull = get(lazyUrl);
        assert(starts == 1);
        abandoned = true;
        auto abandonedWire = get(lazyUrl);
        assert(abandonedWire.size() == abandonedWire.find("\r\n\r\n") + 4);
        assert(starts == 2);
        auto incompatible = store.create({"wrong-rate", 48000, pcm.size(), 1});
        auto badUrl = route.publish("Study", 2, 3, incompatible,
            [&](upnp::StreamRequest &) { return item; });
        auto badWire = get(badUrl);
        assert(badWire.size() == badWire.find("\r\n\r\n") + 4);
        auto throwingUrl = route.publish("Study", 2, 3,
            store.create({"lazy-throw",44100,pcm.size(),1}),
            [](upnp::StreamRequest &) -> std::shared_ptr<Item> {
                throw std::runtime_error("producer unavailable");
            });
        auto failedWire = get(throwingUrl);
        assert(failedWire.size() == failedWire.find("\r\n\r\n") + 4);
        bool callbackReplaced = false;
        try { route.publish("Study", 2, 3, declaration); }
        catch (const std::logic_error &) { callbackReplaced = true; }
        assert(callbackReplaced);
        // Assert the first response bytes precede even a deliberately slow
        // producer resolution. This is a header latency budget, not a claim
        // about hardware's maximum tolerable startup delay.
        bool headersSent = false;
        auto delayedUrl = route.publish("Study", 2, 3,
            store.create({"lazy-delayed",44100,pcm.size(),1}),
            [&](upnp::StreamRequest &) {
                assert(headersSent);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                return item;
            });
        auto begin = std::chrono::steady_clock::now();
        double headerMs = -1, audioMs = -1;
        upnp::HttpRequestIO lazyIO;
        lazyIO.send = [&](const char *, size_t) {
            const auto elapsed = std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            if (!headersSent) headerMs = elapsed;
            else if (audioMs < 0) audioMs = elapsed;
            headersSent = true; return true;
        };
        lazyIO.peerClosed = [] { return false; };
        lazyIO.aborted = [] { return false; };
        lazyIO.disconnect = [] {};
        lazyIO.sendTimeout = [](unsigned) {};
        auto lazyRequest = upnp::httpRequestFromHeaders("GET " + delayedUrl +
            " HTTP/1.1\r\nHost: localhost\r\n\r\n", lazyIO);
        assert(lazyRequest && route.serve(*lazyRequest));
        assert(headerMs >= 0 && headerMs < 100);
        assert(audioMs >= 300 && audioMs < 1000);
        std::cout << "PASS lazy HTTP: headers_ms=" << headerMs
                  << " budget_ms=100 audio_first_byte_ms=" << audioMs << " audio_budget_ms=1000 delayed_producer_ms=300; HEAD/range no start, abandoned headers only, declaration fenced\n";
        std::atomic<unsigned> activations{0};
        auto onceDeclaration = store.create({"lazy-once",44100,pcm.size(),1});
        auto once = std::make_shared<LazyItem>(onceDeclaration, [&] { ++activations; });
        auto onceUrl = route.publish("Study", 2, 3, onceDeclaration,
            [once](upnp::StreamRequest &request) { return once->resolve(request); });
        auto pendingA = std::async(std::launch::async, [&] { return get(onceUrl); });
        auto pendingB = std::async(std::launch::async, [&] { return get(onceUrl, "GET", "bytes=44-"); });
        const auto activateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!activations && std::chrono::steady_clock::now() < activateDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(activations == 1);
        once->publish(item);
        assert(pendingA.get() == lazyFull);
        auto pendingTail = pendingB.get();
        assert(pendingTail.substr(pendingTail.find("\r\n\r\n") + 4) ==
            lazyFull.substr(lazyFull.find("\r\n\r\n") + 4 + 44));
        assert(activations == 1);
        once->cancel();
        auto cancelledWire = get(onceUrl);
        assert(cancelledWire.size() == cancelledWire.find("\r\n\r\n") + 4);
        once->publish(item); // a late completion cannot reactivate this route
        bool badProducer = false;
        try { once->publish(incompatible); } catch (const std::invalid_argument &) { badProducer = true; }
        assert(badProducer);
        auto ready = std::make_shared<LazyItem>(declaration, [&] { ++activations; });
        ready->publish(item); // natural preparation never selects LMS
        assert(ready->resolve(*lazyRequest) == item && activations == 1);
        LazyItem timeout(declaration, [&] { ++activations; });
        auto timeoutStart = std::chrono::steady_clock::now();
        assert(!timeout.resolve(*lazyRequest, std::chrono::milliseconds(20)));
        assert(std::chrono::steady_clock::now() - timeoutStart < std::chrono::seconds(1));
        assert(activations == 2);
        assert(!timeout.resolve(*lazyRequest, std::chrono::milliseconds(1)) && activations == 2);
        std::cout << "PASS lazy activation: concurrent ranges select once; natural prepared source selects zero; cancellation/late producer/timeout fenced\n";
        auto full = get(url);
        assert(lazyFull == full);
        auto body = full.substr(full.find("\r\n\r\n") + 4);
        assert(body.size() == 44 + pcm.size() * 6);
        assert(get(url, "HEAD").find("\r\n\r\n") == get(url, "HEAD").size() - 4);
        auto partial = get(url, "GET", "bytes=45-103");
        assert(partial.find("206 Partial Content") != std::string::npos);
        assert(partial.find("Content-Range: bytes 45-103/264644") != std::string::npos);
        assert(partial.substr(partial.find("\r\n\r\n") + 4) == body.substr(45, 59));
        auto estimated = store.create({"upper",44100,88200,1,2,24,PromiseKind::UpperBound});
        estimated->append(pcm.data(),pcm.size(),false);
        estimated->finish();
        auto upperUrl = route.publish("Study",2,3,estimated);
        auto shortWire = get(upperUrl);
        assert(shortWire.find("Content-Length: 529244\r\n") != std::string::npos);
        assert(shortWire.substr(shortWire.find("\r\n\r\n")+4).size()==264644);
        auto endWire = get(upperUrl,"GET","bytes=264644-");
        assert(endWire.find("206 Partial Content") != std::string::npos);
        assert(endWire.find("Content-Range: bytes 264644-529243/529244\r\n") != std::string::npos);
        assert(endWire.find("Content-Length: 264600\r\n") != std::string::npos);
        assert(endWire.size()==endWire.find("\r\n\r\n")+4);
        auto beyondWire = get(upperUrl,"GET","bytes=300000-");
        assert(beyondWire.size()==beyondWire.find("\r\n\r\n")+4);
        std::cout << "PASS upper-bound HTTP: declared Content-Length, short real body, Range at/beyond real EOF is empty 206 with declared Content-Range\n";
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
        refused = false;
        try {
            route.publish("Study", 2, 3, item);
        } catch (const std::logic_error &) {
            refused = true;
        }
        assert(refused);
        Store other("Kitchen", config);
        auto foreign = other.create({"foreign", 44100, 1, 1});
        refused = false;
        try {
            route.publish("Study", 4, 1, foreign);
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused);
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
