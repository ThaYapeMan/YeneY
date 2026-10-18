// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_actions.h"
#include <stdexcept>
namespace upnp {
SoapResult QueueActions::add(const std::string &uri, const std::string &metadata, unsigned before) {
    return call("AddURIToQueue", {{"InstanceID", "0"},
                                  {"EnqueuedURI", uri},
                                  {"EnqueuedURIMetaData", metadata},
                                  {"DesiredFirstTrackNumberEnqueued", std::to_string(before)},
                                  {"EnqueueAsNext", "0"}});
}
SoapResult QueueActions::addMultiple(const std::vector<std::pair<std::string, std::string>> &items,
                                     uint32_t update, unsigned before) {
    if (items.empty() || items.size() > 16)
        throw std::invalid_argument("queue batch must contain 1..16 items");
    std::string uris, metadata;
    for (const auto &item : items) {
        // Sonos uses a space-delimited URI list and DIDL documents separated
        // by a space. URI spaces must already be escaped by the URL builder.
        if (item.first.find_first_of(" \t\r\n") != std::string::npos)
            throw std::invalid_argument("unescaped queue URI");
        if (!uris.empty()) {
            uris += ' ';
            metadata += ' ';
        }
        uris += item.first;
        metadata += item.second;
    }
    return call("AddMultipleURIsToQueue", {{"InstanceID", "0"},
                                           {"UpdateID", std::to_string(update)},
                                           {"NumberOfURIs", std::to_string(items.size())},
                                           {"EnqueuedURIs", uris},
                                           {"EnqueuedURIsMetaData", metadata},
                                           {"ContainerURI", ""},
                                           {"ContainerMetaData", ""},
                                           {"DesiredFirstTrackNumberEnqueued", std::to_string(before)},
                                           {"EnqueueAsNext", "0"}});
}
SoapResult QueueActions::remove(uint32_t update, unsigned first, unsigned count) {
    if (!first || !count)
        throw std::invalid_argument("queue removal requires a nonempty 1-based range");
    return call("RemoveTrackRangeFromQueue", {{"InstanceID", "0"},
                                              {"UpdateID", std::to_string(update)},
                                              {"StartingIndex", std::to_string(first)},
                                              {"NumberOfTracks", std::to_string(count)}});
}
SoapResult QueueActions::clear() {
    auto result = call("RemoveAllTracksFromQueue", {{"InstanceID", "0"}});
    // 804 is the Sonos empty-queue response, not a generic SOAP success.
    if (result.faultCode == "804")
        result.ok = true;
    return result;
}
SoapResult QueueActions::save(const std::string &title) {
    if (title.empty())
        throw std::invalid_argument("empty queue backup title");
    return call("SaveQueue", {{"InstanceID", "0"}, {"Title", title}, {"ObjectID", ""}});
}
SoapResult QueueActions::selectQueue(const std::string &udn) {
    if (udn.empty() ||
        udn.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
            std::string::npos)
        throw std::invalid_argument("invalid coordinator UDN");
    return call(
        "SetAVTransportURI",
        {{"InstanceID", "0"}, {"CurrentURI", "x-rincon-queue:" + udn + "#0"}, {"CurrentURIMetaData", ""}});
}
SoapResult QueueActions::seekTrack(unsigned number) {
    if (!number)
        throw std::invalid_argument("queue tracks are 1-based");
    return call("Seek", {{"InstanceID", "0"}, {"Unit", "TRACK_NR"}, {"Target", std::to_string(number)}});
}
SoapResult QueueActions::seekTime(const std::string &time) {
    unsigned long long h, m, s;
    char tail;
    if (sscanf(time.c_str(), "%llu:%llu:%llu%c", &h, &m, &s, &tail) != 3 || m >= 60 || s >= 60 || h > 9999)
        throw std::invalid_argument("invalid queue seek time");
    return call("Seek", {{"InstanceID", "0"}, {"Unit", "REL_TIME"}, {"Target", time}});
}
SoapResult QueueActions::play() { return call("Play", {{"InstanceID", "0"}, {"Speed", "1"}}); }
SoapResult QueueActions::pause() { return call("Pause", {{"InstanceID", "0"}}); }
SoapResult QueueActions::stop() { return call("Stop", {{"InstanceID", "0"}}); }
SoapResult QueueActions::position() { return call("GetPositionInfo", {{"InstanceID", "0"}}); }
SoapResult QueueActions::media() { return call("GetMediaInfo", {{"InstanceID", "0"}}); }
SoapResult QueueActions::crossfade() { return call("GetCrossfadeMode", {{"InstanceID", "0"}}); }
SoapResult QueueActions::crossfade(bool enabled) {
    return call("SetCrossfadeMode", {{"InstanceID", "0"}, {"CrossfadeMode", enabled ? "1" : "0"}});
}
} // namespace upnp
