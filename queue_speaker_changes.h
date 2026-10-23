// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "upnp/gena.h"
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
namespace bridge {
// Shared by the event receiver and relay worker; no playback decisions here.
class SpeakerChanges {
    std::mutex mutex;
    std::map<std::string, std::string> values;

public:
    void report(const std::string &source, const std::map<std::string, std::string> &fields) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &field : fields) {
            auto old = values.find(field.first);
            if (old != values.end() && old->second == field.second)
                continue;
            std::ostringstream line;
            line << "yeney: queue speaker-change source=" << source << " field=" << field.first
                 << " old=" << std::quoted(old == values.end() ? "<unknown>" : old->second)
                 << " new=" << std::quoted(field.second);
            auto text = line.str();
            for (char &c : text)
                if (c == '\n' || c == '\r')
                    c = ' ';
            std::printf("%s\n", text.c_str());
            values[field.first] = field.second;
        }
    }
    void event(const upnp::GenaEvent &event) {
        upnp::XmlNode instance;
        if (!upnp::lastChangeInstance(event.body, instance))
            return;
        std::map<std::string, std::string> fields;
        for (const auto *name : {"TransportState", "TransportStatus", "CurrentTrack", "CurrentTrackURI",
                                 "NumberOfTracks", "AVTransportURI"})
            if (const auto node = instance.child(name))
                fields[name] = node->attribute("val");
        if (const auto node = instance.child("CurrentTransportStatus"))
            fields["TransportStatus"] = node->attribute("val");
        report("GENA", fields);
    }
};
} // namespace bridge
