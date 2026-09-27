#pragma once
#include "gena.h"
#include "http_server.h"
#include <thread>
#include "http.h"
#include "timing.h"
#include <algorithm>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
namespace upnp {
inline std::string logValue(std::string value, size_t limit = 4096) {
    value.resize(std::min(value.size(), limit));
    for (char& c : value) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return value;
}
// Route lookup sends no datagrams and re-evaluates the host's current local IP.
inline std::string routeAddress(const std::string& host, unsigned port) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {};
    sockaddr_in peer{}, local{}; peer.sin_family = AF_INET; peer.sin_port = htons(port);
    socklen_t size = sizeof(local); char ip[INET_ADDRSTRLEN]{};
    if (inet_pton(AF_INET, host.c_str(), &peer.sin_addr) == 1
        && connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0
        && getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) == 0)
        inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
    close(fd); return ip;
}
struct SubscriptionHealth {
    std::array<bool, 3> active{};
    std::array<std::string, 3> reason{{"not-subscribed", "not-subscribed", "not-subscribed"}};
};
// Renewal/retry arithmetic is independently testable with a fake clock.
struct SubscriptionSchedule {
    unsigned failures = 0;
    StateClock::time_point due{}, expires{};
    void success(StateClock::time_point now, unsigned seconds) {
        failures = 0; expires = now + std::chrono::seconds(seconds);
        due = now + std::chrono::duration_cast<StateClock::duration>(std::chrono::seconds(seconds)) / 2;
    }
    void failure(StateClock::time_point now) { due = now + (++failures == 1 ? timing::retryFirst : timing::retryLater); expires = now; }
};
class Subscriptions {
public:
    using Targets = std::function<std::array<std::string, 3>()>;
    using Address = std::function<std::string(const std::string&, unsigned)>;
    Subscriptions(unsigned speakerPort, HttpServer& server, Targets targets, GenaHandler handler,
                  Address address = routeAddress)
        : port(speakerPort), server(server), targets(std::move(targets)), handler(std::move(handler)), address(std::move(address)) {
        server.setNotifyHandler([this](const GenaEvent& event) { return receive(event); });
        try { worker = std::thread([this] { run(); }); }
        catch (...) { server.setNotifyHandler({}); throw; }
    }
    ~Subscriptions() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        server.setNotifyHandler({});
    }
    SubscriptionHealth health() const {
        const auto desired = targets();
        std::lock_guard<std::mutex> lock(mutex);
        SubscriptionHealth result;
        for (size_t i = 0; i < slots.size(); ++i) {
            result.active[i] = !slots[i].sid.empty() && slots[i].host == desired[i] && StateClock::now() < slots[i].schedule.expires;
            result.reason[i] = result.active[i] ? "" : slots[i].reason.empty() ? "expired-or-moved" : slots[i].reason;
        }
        return result;
    }
private:
    struct Slot {
        std::string host, address, sid, reason = "not-subscribed";
        SubscriptionSchedule schedule;
        bool subscribing = false, first = true;
    };
    unsigned port;
    HttpServer& server;
    Targets targets;
    GenaHandler handler;
    Address address;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::array<Slot, 3> slots;
    bool stopping = false;
    std::thread worker;
    void unsubscribe(const Slot& slot, Service service) {
        if (!slot.sid.empty()) httpRequest("UNSUBSCRIBE", {slot.host, subscriptionPath(service), port},
                                          {{"SID", slot.sid}}, "", timing::unsubscribeMs);
    }
    bool receive(const GenaEvent& event) {
        const auto desired = targets();
        {
            std::unique_lock<std::mutex> lock(mutex);
            auto& slot = slots[serviceIndex(event.service)];
            if (slot.subscribing && slot.sid.empty())
                wake.wait_for(lock, timing::sidRace, [&] { return stopping || !slot.subscribing; });
            if (stopping || slot.host != desired[serviceIndex(event.service)] || slot.sid.empty() || slot.sid != event.sid || StateClock::now() >= slot.schedule.expires) return false;
            if (slot.first) {
                slot.first = false;
                printf("yeney: first NOTIFY %s body=%s\n", serviceName(event.service), logValue(event.body).c_str());
            }
        }
        return handler(event);
    }
    void run() {
        for (;;) {
            const auto desired = targets();
            for (size_t i = 0; i < slots.size(); ++i) {
                const auto service = static_cast<Service>(i);
                Slot before;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping) break;
                    before = slots[i];
                }
                const bool moved = before.host != desired[i];
                if (!moved && StateClock::now() < before.schedule.due) continue;
                const auto local = address(desired[i], port);
                const bool fresh = moved || before.sid.empty() || before.address != local;
                if (fresh) {
                    { std::lock_guard<std::mutex> lock(mutex); slots[i].sid.clear(); slots[i].reason = "resubscribing"; }
                    unsubscribe(before, service);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    slots[i].host = desired[i]; slots[i].address = local;
                    slots[i].subscribing = fresh;
                    if (fresh) slots[i].first = true;
                }
                std::map<std::string,std::string> headers{{"TIMEOUT", "Second-" + std::to_string(timing::subscriptionSeconds)}};
                if (!fresh) headers["SID"] = before.sid;
                else {
                    headers["NT"] = "upnp:event";
                    headers["CALLBACK"] = "<http://" + local + ":" + std::to_string(server.port()) + eventPath(service) + ">";
                }
                HttpResponse response;
                if (local.empty() || desired[i].empty()) response.error = "no-local-address";
                else response = httpRequest("SUBSCRIBE", {desired[i], subscriptionPath(service), port}, headers, "", timing::subscribeMs);
                unsigned seconds = 0; const auto timeout = response.headers.find("timeout");
                if (timeout != response.headers.end()) {
                    if (timeout->second == "Second-infinite") seconds = timing::subscriptionSeconds;
                    else if (timeout->second.compare(0, 7, "Second-") == 0) {
                        char* end = nullptr; const auto number = std::strtoul(timeout->second.c_str() + 7, &end, 10);
                        if (* (timeout->second.c_str() + 7) && !*end && number <= UINT32_MAX) seconds = number;
                    }
                }
                const auto sid = response.headers.find("sid");
                const bool ok = response.error.empty() && response.status == 200 && seconds
                    && sid != response.headers.end() && !sid->second.empty();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto& slot = slots[i]; slot.subscribing = false;
                    if (ok) {
                        slot.sid = sid->second; slot.reason.clear(); slot.first = true; slot.schedule.success(StateClock::now(), seconds);
                        printf("yeney: subscription service=%s action=%s sid=%s ttl=%u\n", serviceName(service), fresh ? "subscribe" : "renew", logValue(slot.sid).c_str(), seconds);
                    } else {
                        slot.sid.clear(); slot.reason = response.error.empty() ? "HTTP-" + std::to_string(response.status) : logValue(response.error);
                        slot.schedule.failure(StateClock::now());
                        printf("yeney: subscription service=%s action=failed reason=%s\n", serviceName(service), slot.reason.c_str());
                    }
                }
                wake.notify_all();
            }
            std::unique_lock<std::mutex> lock(mutex);
            if (stopping) {
                const auto copy = slots; lock.unlock();
                for (size_t i = 0; i < copy.size(); ++i) unsubscribe(copy[i], static_cast<Service>(i));
                return;
            }
            wake.wait_for(lock, std::chrono::milliseconds(timing::acceptMs), [&] { return stopping; });
        }
    }
};
}
