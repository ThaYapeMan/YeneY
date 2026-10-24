// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::publish() {
    ModeStatus v;
    v.requested = v.active = "queue";
    v.reason = pathReason;
    v.path = pathReason == "remote-source" ? "stream" : "relay";
    v.state = state;
    v.owned = ownership.active();
    v.occurrence = current;
    v.lastFault = fault;
    for (const auto &item : items) {
        v.ownedUris.push_back(item.uri);
        if (item.stream == current) {
            v.currentUri = item.uri;
            v.renderedRate = item.item->descriptor().rate;
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
