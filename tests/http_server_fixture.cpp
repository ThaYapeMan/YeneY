#include "upnp/http_server.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
using namespace upnp;
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    HttpServer server(0);
    server.registerStream("fixture", "/stream", "", "", "", [](StreamRequest& request) {
        const auto mode = request.parameter("mode");
        if (request.method() == StreamRequest::Method::Head) { request.reply(200, "audio/flac"); return true; }
        if (mode == "hold" || mode == "closed") {
            const std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            request.send(wire.data(),wire.size());
            while (!request.peerClosed() && !request.aborted()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (mode == "closed") puts("PEER CLOSED");
        } else if (mode == "slow") {
            request.sendTimeout(100);
            const std::string data(1024*1024, 'x');
            const auto start = std::chrono::steady_clock::now();
            while (request.send(data.data(), data.size()) && !request.aborted()) {}
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
            printf("SEND TIMEOUT %lld\n", (long long)ms);
        } else {
            const auto body = request.parameter("value") + "|" + request.header("x-MiXeD");
            const auto wire = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            request.send(wire.data(), wire.size());
        }
        return true;
    });
    server.setNotifyHandler([](const GenaEvent& event) {
        if (event.sid != "uuid:test" || event.sequence != 7) return false;
        if (event.service == Service::AVTransport) return event.state == "PLAYING";
        if (event.service == Service::RenderingControl) return event.update.volume == 26;
        return event.topology.size() == 3 && event.topology.front().name == "Study";
    });
    std::cout << "PORT " << server.port() << std::endl;
    std::cin.get();
}
