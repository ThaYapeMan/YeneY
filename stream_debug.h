#pragma once
#include "upnp/stream_server.h"
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
// Per-response measurements; no TCP_INFO calls or logging when disabled.
class StreamDebugRequest : public upnp::StreamRequest {
    upnp::StreamRequest& request;
    std::function<std::chrono::steady_clock::time_point()> now;
    bool enabled, begun = false;
    unsigned next = 1;
    uint64_t sent = 0, wireBase = UINT64_MAX;
    double longest = 0;
    std::chrono::steady_clock::time_point started;
    std::function<uint64_t()> audio;
    uint64_t base = 0;
    void line(bool final) {
        if (!enabled || !begun) return;
        const double seconds = std::chrono::duration<double>(now()-started).count();
        if (!final && seconds < next) return;
        const auto lead = int64_t(audio ? audio() - base : 0) - int64_t(seconds*1000);
        const uint64_t wire = request.connectionSentBytes();
        if (wire != UINT64_MAX) { sent = wire - wireBase; wireBase = wire; }
        printf("stream %s: debug t=%.3f lead_ms=%lld bytes=%llu send_max_ms=%.3f %s%s\n",
            parameter("stream").c_str(), seconds, (long long)lead, (unsigned long long)sent,
            longest, request.connectionDiagnostics().c_str(), final ? " final" : "");
        sent = 0; longest = 0;
        next = seconds < 30 ? unsigned(seconds)+1 : (unsigned(seconds)/10+1)*10;
    }
public:
    explicit StreamDebugRequest(upnp::StreamRequest& request,
        std::function<std::chrono::steady_clock::time_point()> clock = std::chrono::steady_clock::now)
        : request(request), now(std::move(clock)) {
        const char* value = std::getenv("YENEY_DEBUG_STREAM"); enabled = value && !strcmp(value,"1");
    }
    ~StreamDebugRequest() { if (enabled && begun) { request.peerClosed(); line(true); } }
    void encoder(std::function<uint64_t()> clock, bool continuation = false) {
        audio = std::move(clock); base = continuation ? audio() : 0;
    }
    std::string path() const override { return request.path(); }
    Method method() const override { return request.method(); }
    std::string parameter(const std::string& name) const override { return request.parameter(name); }
    upnp::RequestHeaders headers() const override { return request.headers(); }
    std::string serverName() const override { return request.serverName(); }
    std::string connectionDiagnostics() const override { return request.connectionDiagnostics(); }
    bool aborted() const override { return request.aborted(); }
    void sendTimeout(unsigned ms) override { request.sendTimeout(ms); }
    void reply(unsigned status,const std::string& type = {}) override { request.reply(status,type); }
    void disconnect() override { request.disconnect(); }
    bool peerClosed() override { const bool closed = request.peerClosed(); line(false); return closed; }
    bool send(const char* data,size_t size) override {
        if (!enabled) return request.send(data,size);
        if (!begun) { begun = true; wireBase = request.connectionSentBytes(); started = now(); }
        const auto before = now();
        const bool ok = request.send(data,size);
        longest = std::max(longest,std::chrono::duration<double,std::milli>(now()-before).count());
        if (ok) sent += size;
        line(false); return ok;
    }
};
