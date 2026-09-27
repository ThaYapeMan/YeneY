#include "http_server.h"
#include "timing.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace upnp {
namespace {
// Preserve the headers of the existing stream writer and noson reply helper.
constexpr char streamServerName[] = "libnoson/2.13.2";
constexpr char replyServerName[] = "SONOS/2.13.2";
std::string lower(std::string text) {
    for (auto& c : text) c = std::tolower(static_cast<unsigned char>(c));
    return text;
}
std::string decode(const std::string& text) {
    auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i+1]) >= 0 && hex(text[i+2]) >= 0) {
            out += char(hex(text[i+1]) * 16 + hex(text[i+2])); i += 2;
        } else out += text[i] == '+' ? ' ' : text[i];
    }
    return out;
}
const char* reason(unsigned status) {
    switch (status) {
    case 200: return "OK"; case 400: return "Bad Request"; case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
    case 412: return "Precondition Failed"; case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable"; default: return "Internal Server Error";
    }
}
std::string response(unsigned status, size_t size = 0, const std::string& type = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\nServer: " + replyServerName
        + "\r\nContent-Length: " + std::to_string(size) + (type.empty() ? "" : "\r\nContent-Type: " + type)
        + "\r\nConnection: close\r\n";
}
struct Request : StreamRequest {
    std::string verb, uri, version;
    RequestHeaders fields;
    std::map<std::string, std::string> values, params;
    HttpRequestIO io;
    explicit Request(HttpRequestIO callbacks) : io(std::move(callbacks)) {}
    bool parse(const std::string& wire) {
        const auto end = wire.find("\r\n\r\n"), first = wire.find("\r\n");
        if (end == std::string::npos || end + 4 > timing::httpHeaderBytes || first > end) return false;
        std::istringstream line(wire.substr(0, first)); std::string extra;
        if (!(line >> verb >> uri >> version) || line >> extra || uri.empty() || uri[0] != '/'
            || (version != "HTTP/1.1" && version != "HTTP/1.0")) return false;
        for (size_t at = first + 2; at < end;) {
            const auto stop = wire.find("\r\n", at), colon = wire.find(':', at);
            if (colon == std::string::npos || colon >= stop || colon == at) return false;
            const auto name = wire.substr(at, colon - at);
            for (unsigned char c : name) if (!std::isalnum(c) && std::string("!#$%&'*+-.^_`|~").find(c) == std::string::npos) return false;
            auto value = wire.substr(colon + 1, stop - colon - 1);
            for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return false;
            const auto a = value.find_first_not_of(" \t"), b = value.find_last_not_of(" \t");
            value = a == std::string::npos ? "" : value.substr(a, b - a + 1);
            if (!values.emplace(lower(name), value).second) return false;
            fields.emplace_back(name, value); at = stop + 2;
        }
        auto query = uri.find('?');
        if (query != std::string::npos) {
            const auto text = uri.substr(query + 1); uri.resize(query);
            for (size_t at = 0; at <= text.size();) {
                auto stop = text.find('&', at); if (stop == std::string::npos) stop = text.size();
                const auto token = text.substr(at, stop-at); const auto equal = token.find('=');
                if (equal != std::string::npos) params.emplace(decode(token.substr(0,equal)), decode(token.substr(equal+1)));
                if (stop == text.size()) break;
                at = stop + 1;
            }
        }
        return true;
    }
    std::string path() const override { return uri; }
    Method method() const override { return verb == "GET" ? Method::Get : verb == "HEAD" ? Method::Head : Method::Other; }
    std::string parameter(const std::string& key) const override { auto i=params.find(key); return i == params.end() ? "" : i->second; }
    RequestHeaders headers() const override { return fields; }
    bool send(const char* data, size_t n) override { return io.send(data,n); }
    bool peerClosed() override { return io.peerClosed(); }
    void sendTimeout(unsigned ms) override { io.sendTimeout(ms); }
    void disconnect() override { io.disconnect(); }
    bool aborted() const override { return io.aborted && io.aborted(); }
    std::string serverName() const override { return streamServerName; }
    void reply(unsigned status, const std::string& type) override {
        const auto wire = response(status, 0, type) + "\r\n"; send(wire.data(), wire.size());
    }
};
struct Socket {
    const int fd;
    explicit Socket(int descriptor) : fd(descriptor) { if (fd < 0) throw std::runtime_error(strerror(errno)); }
    ~Socket() { close(fd); }
    Socket(const Socket&) = delete;
    void disconnect() { shutdown(fd, SHUT_RDWR); }
    void timeout(unsigned ms) {
        timeval t{time_t(ms/1000), suseconds_t(ms%1000*1000)};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
    }
    bool send(const char* bytes, size_t size) {
        while (size) {
            const auto n = ::send(fd, bytes, size, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            bytes += n; size -= n;
        }
        return true;
    }
    bool closed() {
        char byte; const auto n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        return n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK);
    }
};
}
std::unique_ptr<StreamRequest> httpRequestFromHeaders(const std::string& headers, HttpRequestIO io) {
    auto request = std::make_unique<Request>(std::move(io));
    if (!request->parse(headers)) return {};
    return request;
}
struct HttpServer::Impl {
    struct Route { StreamResource resource; Handler handler; };
    struct Worker { std::shared_ptr<Socket> socket; std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::unique_ptr<Socket> listener;
    unsigned boundPort = 0;
    std::atomic<bool> stopping{false};
    std::thread acceptThread;
    std::mutex routesMutex;
    std::map<std::string, Route> routes;
    std::vector<Worker> workers; // owned by accept thread; joined before destruction
    Impl(unsigned first, unsigned attempts) {
        if (first > 65535 || !attempts || attempts > 65536 - first) throw std::runtime_error("invalid HTTP port range");
        listener = std::make_unique<Socket>(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        int reuse = 1; setsockopt(listener->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        bool bound = false;
        for (unsigned n = 0; n < attempts; ++n) {
            addr.sin_port = htons(first + n);
            if (!bind(listener->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) { bound = true; break; }
            if (!first) break;
        }
        socklen_t size = sizeof(addr);
        if (!bound || listen(listener->fd, timing::httpConnections) || getsockname(listener->fd, reinterpret_cast<sockaddr*>(&addr), &size))
            throw std::runtime_error(strerror(errno));
        boundPort = ntohs(addr.sin_port);
        acceptThread = std::thread([this] { run(); });
    }
    ~Impl() { stopping = true; listener->disconnect(); if (acceptThread.joinable()) acceptThread.join(); }
    void run() {
        while (!stopping) {
            for (auto i = workers.begin(); i != workers.end();) {
                if (*i->done) { i->thread.join(); i = workers.erase(i); } else ++i;
            }
            pollfd p{listener->fd, POLLIN, 0};
            if (::poll(&p, 1, timing::acceptMs) <= 0) continue;
            const int fd = accept4(listener->fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) continue;
            auto client = std::make_shared<Socket>(fd); client->timeout(timing::httpSendMs);
            if (workers.size() >= timing::httpConnections) {
                const auto wire = response(503) + "\r\n"; client->send(wire.data(), wire.size()); continue;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            try {
                // Reserve before starting a joinable thread: allocation failure
                // must never destroy an unjoined std::thread.
                workers.reserve(timing::httpConnections);
                workers.push_back({client, std::thread([this, client, done] {
                    try { serve(client); } catch (...) { client->disconnect(); }
                    *done = true;
                }), done});
            } catch (...) { client->disconnect(); }
        }
        for (auto& w : workers) w.socket->disconnect();
        for (auto& w : workers) w.thread.join();
        workers.clear();
    }
    void serve(const std::shared_ptr<Socket>& client) {
        const auto deadline = std::chrono::steady_clock::now() + timing::httpHeaderDeadline;
        std::string wire;
        unsigned error = 0;
        while (wire.find("\r\n\r\n") == std::string::npos && !stopping) {
            if (wire.size() >= timing::httpHeaderBytes) { error = 431; break; }
            if (std::chrono::steady_clock::now() >= deadline) { error = 408; break; }
            pollfd p{client->fd, POLLIN, 0};
            if (::poll(&p, 1, timing::receiveMs) <= 0) continue;
            char bytes[4096]; const auto n = recv(client->fd, bytes, std::min(sizeof(bytes), timing::httpHeaderBytes - wire.size()), MSG_DONTWAIT);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) return;
            wire.append(bytes, n);
        }
        if (stopping) return;
        HttpRequestIO io{
            [client](const char* p,size_t n) { return client->send(p,n); },
            [client] { return client->closed(); }, [this] { return stopping.load(); },
            [client](unsigned ms) { client->timeout(ms); }, [client] { client->disconnect(); }};
        auto request = httpRequestFromHeaders(wire, std::move(io));
        if (error || !request) {
            const auto reply = response(error ? error : 400) + "\r\n"; client->send(reply.data(),reply.size()); return;
        }
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            for (const auto& r : routes) if (r.second.resource.uri == request->path()) { handler = r.second.handler; break; }
        }
        if (!handler || !handler(*request)) request->reply(404);
    }
};
HttpServer::HttpServer(unsigned port, unsigned attempts) : impl(new Impl(port, attempts)) {}
HttpServer::~HttpServer() = default;
StreamResource HttpServer::registerStream(const std::string& name, const std::string& path, const std::string&,
    const std::string&, const std::string&, Handler handler) {
    std::lock_guard<std::mutex> lock(impl->routesMutex);
    auto& route = impl->routes[name]; route.resource = {path, {}}; route.handler = std::move(handler);
    return route.resource;
}
StreamResource HttpServer::resource(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl->routesMutex);
    const auto i = impl->routes.find(name); return i == impl->routes.end() ? StreamResource{} : i->second.resource;
}
unsigned HttpServer::port() { return impl->boundPort; }
}
