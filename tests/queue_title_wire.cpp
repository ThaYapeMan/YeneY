// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "upnp/queue_coordinator.h"
#include "upnp/queue_metadata.h"
#include <cassert>
int main(int argc, char **argv) {
    assert(argc == 2);
    const auto location = std::string("http://127.0.0.1:") + argv[1] + "/description.xml";
    upnp::Speaker room;
    room.ip = "127.0.0.1"; room.uuid = "RINCON_TEST";
    room.name = room.coordinator = "Study"; room.members = {"Study"};
    room.location = location;
    upnp::QueueCoordinator coordinator(room);
    upnp::QueueActions actions([&](const auto &name, const auto &args) {
        return coordinator.transport(name, args);
    });
    upnp::QueueMetadata m;
    m.url = "http://127.0.0.1:1401/music/queue.wav?session=title-fixture&epoch=1&item=1&revision=1";
    m.session = "title-fixture";
    m.title = "When I Fall In Love & <live>";
    m.artist = "Michael Bublé";
    m.album = "love (Deluxe Edition)";
    m.artwork = "http://127.0.0.1:9000/music/123/cover.jpg?x=1&y=2";
    m.frames = 44100 * 65;
    m.rate = 44100;
    m.size = 44 + 6 * m.frames;
    assert(actions.add(m.url, upnp::queueDidl(m)).response.value("NumTracksAdded") == "1");
}
