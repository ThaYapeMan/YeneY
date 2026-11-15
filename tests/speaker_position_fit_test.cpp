// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_position.h"
#include "position_settings.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <unistd.h>
int main() {
    setenv("YENEY_CONFIG", "/dev/null", 1);
    unsetenv("YENEY_POSITION_SOURCE");
    assert(bridge::positionSetting("Study") == "auto");
    setenv("YENEY_POSITION_SOURCE", "upnp", 1);
    assert(bridge::positionSetting("Study") == "upnp");
    assert(bridge::positionSetting("Study", "websocket") == "websocket");
    unsetenv("YENEY_POSITION_SOURCE");
    const auto temporary = std::getenv("TMPDIR");
    const auto config = std::filesystem::path(temporary ? temporary : "/var/tmp") /
        ("position-settings-" + std::to_string(getpid()));
    { std::ofstream file(config); file << "YENEY_POSITION_SOURCE=upnp\nroom.Study.YENEY_POSITION_SOURCE=websocket\n"; }
    setenv("YENEY_CONFIG", config.c_str(), 1);
    assert(bridge::positionSetting("Study") == "websocket");
    assert(bridge::positionSetting("Port") == "upnp");
    setenv("YENEY_POSITION_SOURCE", "auto", 1);
    assert(bridge::positionSetting("Study") == "auto");
    assert(bridge::positionSetting("Study", "upnp") == "upnp");
    std::filesystem::remove(config);
    std::cout << "PASS T2 room/global precedence, environment and CLI override\n";
    struct FutureSource : bridge::SpeakerPosition {
        bool ready = false;
        const char *name() const override { return "future"; }
        const char *precision() const override { return "ms"; }
        std::optional<bridge::SpeakerPositionSample> position(double now) const override {
            return ready ? std::optional<bridge::SpeakerPositionSample>({"one", "PLAYING", 1000, now, 1}) : std::nullopt;
        }
    } future;
    bridge::UpnpSpeakerPosition upnp;
    upnp.update({"one", "PLAYING", 1000, 1, 10});
    bridge::PositionSources sources;
    sources.add(future); sources.add(upnp);
    assert(std::string(sources.select(1).first->name()) == "upnp");
    future.ready = true;
    assert(std::string(sources.select(1).first->name()) == "future");
    std::cout << "PASS T2 selection: default auto, environment, CLI override, ordered registration accepts a third source and fallback\n";
    bridge::PositionFit fit;
    assert(!fit.position(0));
    assert(fit.add({"one", "PLAYING", 1000, 1, 10}));
    assert(std::abs(fit.position(1.1)->positionMs - 1100) < .001);
    for (int i = 1; i <= 20; ++i)
        assert(fit.add({"one", "PLAYING", 1000.0 + i * 200, 1.0 + i * .2, i % 5 ? 10.0 : 50.0}));
    const auto before = fit.position(5.1)->positionMs;
    assert(!fit.add({"one", "PLAYING", 9000, 5.1, 201}));
    assert(std::abs(fit.position(5.1)->positionMs - before) < 5);
    assert(!fit.add({"one", "PLAYING", 5600, 5.2, 10}));
    assert(fit.add({"one", "PLAYING", 5800, 5.4, 10}));
    assert(std::abs(fit.position(5.4)->positionMs - 5800) < .001);
    assert(fit.add({"two", "PLAYING", 0, 5.5, 10}));
    assert(fit.position(5.5)->positionMs == 0);
    assert(fit.position(5.55)->observedAt == 5.5);
    assert(fit.position(5.55)->sampleTime == 5.55);
    assert(fit.add({"two", "PAUSED", 100, 5.6, 10}));
    assert(fit.position(10)->positionMs == 100);
    std::cout << "PASS T8 fit: RTT >200 ms discarded; outlier movement <5 ms; confirmed 400 ms seek reset in 200 ms; first sample, item and pause reset\n";
}
