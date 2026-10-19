// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "discovery.h"
#include "queue_actions.h"
#include "queue_ownership.h"
#include <map>
namespace upnp {
// Queue-only SOAP adapter. Construct/use on the room controller's worker.
// Endpoints come from the selected coordinator's advertised description.
class QueueCoordinator {
    struct Endpoint {
        std::string urn;
        HttpUrl url;
    };
    Speaker room;
    std::map<std::string, Endpoint> services;
    std::function<void(const std::string &)> log;
    unsigned timeout;
    void discover();
    SoapResult raw(const std::string &, const std::string &, const SoapArguments &);
    void fence();

public:
    explicit QueueCoordinator(Speaker, std::function<void(const std::string &)> = {},
                              unsigned timeoutMs = 2000);
    SoapResult transport(const std::string &, const SoapArguments &);
    QueueSnapshot browse();
    std::vector<SavedQueue> playlists();
    void destroyPlaylist(const std::string &id);
};
} // namespace upnp
