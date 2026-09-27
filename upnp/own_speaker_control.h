#pragma once
#include "speaker_control.h"
#include "soap.h"
#include "gena.h"
#include "subscriptions.h"
#include "monitor.h"
#include "targets.h"
#include <condition_variable>
#include <memory>
#include <chrono>
#include <functional>
#include <mutex>
namespace upnp {
struct StreamActivity { bool streaming = false, requestOpen = false; };
class OwnSpeakerControl : public SpeakerControl {
public:
    explicit OwnSpeakerControl(std::function<unsigned()> streamPort, unsigned speakerPort = 1400,
                               std::function<StreamActivity()> activity = {},
                               std::function<void()> eventCallback = {});
    ~OwnSpeakerControl() override;
    void shutdownEvents();
    bool discover(const std::string&, const std::string& = {}) override;
    std::vector<std::string> discoverRooms(const std::string& = {}) override;
    std::vector<Speaker> discoverRoomDetails(const std::string& = {}) override;
    Speaker speaker() const override;
    bool playStream(const std::string&, const std::string&, const std::string& = {}) override;
    bool play() override;
    bool pause() override;
    bool stop() override;
    TransportInfo transportInfo() override;
    bool readTransportInfo(TransportInfo&) override;
    uint8_t displayVolume() override;
    static unsigned actionTimeoutMs(const std::string& action);
    bool positionInfo(uint32_t&, std::string* = nullptr) override;
    bool currentUri(std::string&) override;
    std::string controllerUri() override;
    void poll() override;
    unsigned pollIntervalMs() const override { return timing::pollMs; }
private:
    using Clock = std::chrono::steady_clock;
    std::function<unsigned()> streamPort;
    unsigned speakerPort;
    std::function<StreamActivity()> streamActivity;
    SpeakerStateStore state;
    std::mutex runtimeMutex;
    std::string localAddress, room, sentTitle, sentUri, sentUrl;
    Clock::time_point positionAt{};
    bool positionReading = false, freshStreamPosition = false, pauseTimeoutLogged = false;
    MonitorPolicy monitor;
    std::string monitorLog;
    std::function<void()> eventCallback;
    std::unique_ptr<Subscriptions> subscriptions;
    void startEvents();
    bool receiveEvent(const GenaEvent&);
    void apply(StateUpdate);
    SoapResult call(const std::string& action, const SoapArguments& args,
                    const std::string& host = {}, const std::string& service = "AVTransport");
    bool topology(const std::string& host, bool initial);
};
}
