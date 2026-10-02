#include "timing_probe_runtime.h"
#include "upnp/own_speaker_control.h"
#include <atomic>
#include <cassert>
#include <thread>
int main(int argc, char **argv) {
    assert(argc == 3);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string mode = argv[2];
    std::atomic<bool> allowed(mode != "relinquished" && mode != "foreign");
    auto &d = timing_probe::diagnostics();
    d.anchor(7, 0);
    d.handed(7, 0, 44100);
    upnp::OwnSpeakerControl control([] { return 1450; }, std::stoul(argv[1]),
                                    [&] { return upnp::StreamActivity{true, true, allowed.load()}; });
    assert(control.discover("Study", "127.0.0.1"));
    uint32_t initial = 0, cached = 0;
    const bool initialKnown = control.positionInfo(initial);
    const std::string uri = "http://127.0.0.1:1450/yeney.flac?session=abcdef&stream=" +
                            std::string(mode == "mismatch" ? "8" : "7");
    assert(control.playStream(uri, "Test"));
    if (mode == "foreign") {
        std::string uri;
        assert(control.currentUri(uri));
        allowed.store(true);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    assert(control.positionInfo(cached) == initialKnown && (!initialKnown || initial == cached));
    if (mode == "active" || mode == "errors")
        std::this_thread::sleep_for(std::chrono::milliseconds(10200));
    upnp::TransportInfo info;
    assert(control.readTransportInfo(info)); // mock now pauses; worker must stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    control.shutdownEvents();
    puts("PASS: probe cannot mutate the leased position cache; orderly worker shutdown");
}
