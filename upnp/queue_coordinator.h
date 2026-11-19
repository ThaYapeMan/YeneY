// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "discovery.h"
#include "queue_actions.h"
#include "queue_ownership.h"
#include <map>
#include <stdexcept>
namespace upnp {
struct QueueReadUnanswered : std::runtime_error {
    std::string action;
    QueueReadUnanswered(const std::string &action, const std::string &error)
        : std::runtime_error(error.empty() ? "no HTTP status" : error), action(action) {}
};

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
    bool identityConfirmed = false, reuseIdentity = false;
    bool reading = false;
    std::function<void(const QueueReadUnanswered &)> waitForRead;
    std::function<void()> answered;
    std::function<unsigned(unsigned)> readDeadline;
    void discover();
    SoapResult raw(const std::string &, const std::string &, const SoapArguments &);
    SoapResult request(const std::string &, const std::string &, const SoapArguments &, bool readOnly);
    QueueSnapshot browseOnce();
    std::vector<SavedQueue> playlistsOnce();
    void fence();

public:
    explicit QueueCoordinator(Speaker, std::function<void(const std::string &)> = {},
                              unsigned timeoutMs = 2000);
    void readRecovery(std::function<void(const QueueReadUnanswered &)> wait,
                      std::function<void()> answer, std::function<unsigned(unsigned)> deadline) {
        waitForRead = std::move(wait);
        answered = std::move(answer);
        readDeadline = std::move(deadline);
    }
    // A retry scope may end in a write. No write may precede another read
    // inside that scope because a later unanswered read restarts the scope.
    template<class F> auto read(F operation) -> decltype(operation()) {
        if (reading || !waitForRead) return operation();
        reading = true;
        struct Reset { bool &flag; ~Reset() { flag = false; } } reset{reading};
        for (;;) {
            try {
                return operation();
            } catch (const QueueReadUnanswered &e) {
                waitForRead(e);
            }
        }
    }
    SoapResult transport(const std::string &, const SoapArguments &);
    // Cleanup requires both live coordinator identity and our queue transport.
    SoapResult releaseTransport(const std::string &, const SoapArguments &);
    // Refresh topology at ownership acquisition; reuse it within the owned sequence.
    Speaker currentRoom();
    void reuseOwnershipIdentity() { reuseIdentity = true; }
    void invalidateIdentity() { identityConfirmed = reuseIdentity = false; }
    QueueSnapshot browse();
    std::vector<SavedQueue> playlists();
    void destroyPlaylist(const std::string &id);
};
} // namespace upnp
