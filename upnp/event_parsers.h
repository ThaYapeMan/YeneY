#pragma once
#include "speaker_state.h"
#include "xml.h"
#include "discovery.h"
namespace upnp {
inline std::string xmlLocalName(const XmlNode& node) {
    const auto colon = node.name.find(':');
    return colon == std::string::npos ? node.name : node.name.substr(colon + 1);
}
inline bool lastChangeInstance(const std::string& body, XmlNode& instance) {
    XmlNode properties, change;
    if (!parseXml(body, properties) || xmlLocalName(properties) != "propertyset") return false;
    for (const auto& p : properties.children) {
        const auto last = p.child("LastChange");
        if (xmlLocalName(p) != "property" || !last) continue;
        if (!parseXml(last->text, change) || xmlLocalName(change) != "Event") return false;
        for (const auto& i : change.children)
            if (xmlLocalName(i) == "InstanceID" && i.attribute("val") == "0") { instance = i; return true; }
    }
    return false;
}
inline bool parseAvTransport(const std::string& body, StateUpdate& result) {
    XmlNode i;
    if (!lastChangeInstance(body, i)) return false;
    StateUpdate u; u.source = Source::Event;
    auto val = [&](const char* name, std::optional<std::string>& field) {
        const auto node = i.child(name); if (node) field = node->attribute("val");
    };
    val("TransportState", u.state); val("TransportStatus", u.status);
    val("CurrentTransportStatus", u.status); val("AVTransportURI", u.uri);
    val("CurrentTrackURI", u.trackUri); val("CurrentTrackDuration", u.duration);
    if (u.state && (u.state->empty() || u.state->find_first_of("\r\n") != std::string::npos)) return false;
    if (u.status && u.status->find_first_of("\r\n") != std::string::npos) return false;
    if (u.state) u.available = true;
    if (const auto metadata = i.child("CurrentTrackMetaData")) {
        const auto text = metadata->attribute("val");
        if (text.empty() || text == "NOT_IMPLEMENTED") u.title = "";
        else {
            XmlNode didl;
            if (!parseXml(text, didl)) return false;
            const auto item = didl.child("item");
            if (item && item->child("title")) u.title = item->value("title");
        }
    }
    result = u; return true;
}
inline bool parseRenderingControl(const std::string& body, StateUpdate& result) {
    XmlNode i;
    if (!lastChangeInstance(body, i)) return false;
    StateUpdate u; u.source = Source::Event;
    for (const auto& node : i.children) if (xmlLocalName(node) == "Volume" && node.attribute("channel") == "Master") {
        const auto value = node.attribute("val"); unsigned parsed = 0;
        if (value.empty()) return false;
        for (char c : value) { if (c < '0' || c > '9' || parsed > 100) return false; parsed = parsed * 10 + c - '0'; }
        if (parsed > 100) return false;
        u.volume = parsed;
    }
    result = u; return true;
}
inline bool parseZoneGroupEvent(const std::string& body, std::vector<Speaker>& speakers) {
    XmlNode properties;
    if (!parseXml(body, properties) || xmlLocalName(properties) != "propertyset") return false;
    for (const auto& p : properties.children) {
        const auto zone = p.child("ZoneGroupState");
        if (xmlLocalName(p) != "property" || !zone) continue;
        XmlNode topology;
        if (!parseXml(zone->text, topology)) return false;
        speakers = parseTopology(zone->text); return !speakers.empty();
    }
    return false;
}
inline bool topologyUpdate(const std::vector<Speaker>& speakers, const std::string& room,
                           const std::string& uuid, StateUpdate& u) {
    Speaker selected;
    if (uuid.empty()) { if (!matchRoom(speakers, room, selected)) return false; }
    else { for (const auto& s : speakers) if (s.uuid == uuid) selected = s; if (selected.uuid.empty()) return false; }
    Group group; group.name = selected.coordinator; group.members = selected.members;
    for (const auto& s : speakers) if (s.name == group.name) { group.uuid = s.uuid; group.ip = s.ip; break; }
    if (group.ip.empty()) return false;
    u.room = selected; u.group = group; return true;
}
}
