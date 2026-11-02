// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::publish() {
    std::vector<std::shared_ptr<queue_audio::Item>> retained;
    for (const auto &entry : items) if (entry.item) retained.push_back(entry.item);
    queue.protectWindow(std::move(retained));
    ModeStatus v;
    v.requested = v.active = "queue";
    v.reason = pathReason;
    v.path = pathReason == "remote-source" ? "stream" : "relay";
    v.state = state;
    v.owned = ownership.active() && !externalReleased;
    v.occurrence = current;
    v.lastFault = fault;
    for (const auto &item : items) {
        if (externalReleased) break;
        v.ownedUris.push_back(item.uri);
        if (item.stream == current) {
            v.currentUri = item.uri;
            v.title = item.title;
            v.renderedRate = item.item ? item.item->descriptor().rate : 0;
        }
    }
    if (publishedState != v.state || publishedUri != v.currentUri) {
        logQueue("relay-change state=" + publishedState + "->" + v.state + " current=" + publishedUri + "->" +
                 v.currentUri);
        publishedState = v.state;
        publishedUri = v.currentUri;
    }
    status.publish(std::move(v));
    // Bypass the periodic stream sampler's one-second throttle. Snapshot
    // publication happens in the same call as every relay state update.
    streamMetrics().publishPlayback();
}
} // namespace bridge
