// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_item_store.h"
#include "upnp/stream_server.h"
#include <map>
#include <mutex>
namespace queue_audio {
// One instance belongs to one room and one process session. Readers retain
// immutable assets independently; revoking an epoch prevents new readers.
class Route {
    struct State;
    std::shared_ptr<State> state;
    static bool serve(const std::shared_ptr<State> &, upnp::StreamRequest &);

public:
    Route(std::string room, std::string session);
    void registerWith(upnp::StreamServer &);
    std::string publish(const std::string &room, uint64_t epoch, uint64_t revision,
                        const std::shared_ptr<Item> &);
    void revoke(uint64_t epoch);
    bool serve(upnp::StreamRequest &);
};
} // namespace queue_audio
