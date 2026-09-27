#pragma once
#include "stream_server.h"
#include <memory>

namespace upnp {
// The request parser and response writer use the same I/O boundary in the
// listener and deterministic streamer fixtures. Production callbacks own a
// shared RAII socket; none of these callbacks borrow a raw descriptor.
struct HttpRequestIO {
    std::function<bool(const char*, size_t)> send;
    std::function<bool()> peerClosed, aborted;
    std::function<void(unsigned)> sendTimeout;
    std::function<void()> disconnect;
};
std::unique_ptr<StreamRequest> httpRequestFromHeaders(const std::string&, HttpRequestIO);
class HttpServer : public StreamServer {
public:
    // Match noson's 1400..1409 search. Zero requests an ephemeral test port.
    explicit HttpServer(unsigned firstPort = 1400, unsigned attempts = 10);
    ~HttpServer() override;
    StreamResource registerStream(const std::string&, const std::string&, const std::string&,
        const std::string&, const std::string&, Handler) override;
    StreamResource resource(const std::string&) override;
    unsigned port() override;
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
