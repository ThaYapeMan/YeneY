// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "upnp/gena.h"
#include "transport_source.h"
#include <chrono>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
namespace bridge {
// Shared by the event receiver and relay worker; no playback decisions here.
class SpeakerChanges {
    std::mutex mutex;
    std::map<std::string, std::string> values;

    // A response may contain both fresh fields and fields superseded during I/O.
    using Clock = std::chrono::steady_clock;
    std::map<std::string, Clock::time_point> eventTimes;
    std::string sid;
    uint32_t sequence = 0;
    bool sequenced = false;

public:
    std::string foreignTransport(const std::string &queueUri) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto uri = values.find("AVTransportURI");
        return uri != values.end() && !uri->second.empty() && uri->second != queueUri
                   ? transportSourceClass(uri->second) : "";
    }
private:
    void report(const std::string &source, const std::map<std::string, std::string> &fields) {
        for (const auto &field : fields) {
            // Position advances every second; order it without routine poll logging.
            if (field.first == "RelativeTimePosition") {
                values[field.first] = field.second;
                continue;
            }
            auto old = values.find(field.first);
            if (old != values.end() && old->second == field.second)
                continue;
            const bool uri = field.first == "AVTransportURI" || field.first == "CurrentTrackURI";
            const auto display = [&](const std::string &value) { return uri ? transportSourceLog(value) : value; };
            std::ostringstream line;
            line << "yeney: queue speaker-change source=" << source << " field=" << field.first
                 << " old=" << std::quoted(old == values.end() ? "<unknown>" : display(old->second))
                 << " new=" << std::quoted(display(field.second));
            auto text = line.str();
            for (char &c : text)
                if (c == '\n' || c == '\r')
                    c = ' ';
            std::printf("%s\n", text.c_str());
            values[field.first] = field.second;
        }
    }
public:
    std::map<std::string, std::string> poll(Clock::time_point started,
                                          std::map<std::string, std::string> fields) {
        std::lock_guard<std::mutex> lock(mutex);
        bool stale = false;
        for (auto &field : fields) {
            const auto stamp = eventTimes.find(field.first);
            if (stamp != eventTimes.end() && started < stamp->second) {
                field.second = values.at(field.first);
                stale = true;
            }
        }
        if (stale)
            std::printf("yeney: queue stale poll answer discarded after GENA seq=%u\n", sequence);
        report("poll", fields);
        return fields;
    }
    void event(const upnp::GenaEvent &event) {
        upnp::XmlNode instance;
        if (!upnp::lastChangeInstance(event.body, instance))
            return;
        std::map<std::string, std::string> fields;
        for (const auto *name : {"TransportState", "TransportStatus", "CurrentTrack", "CurrentTrackURI",
                                 "NumberOfTracks", "AVTransportURI", "RelativeTimePosition"})
            if (const auto node = instance.child(name))
                fields[name] = node->attribute("val");
        if (const auto node = instance.child("CurrentTransportStatus"))
            fields["TransportStatus"] = node->attribute("val");
        std::lock_guard<std::mutex> lock(mutex);
        // SEQ is scoped to the subscription and wraps as an unsigned counter.
        if (sequenced && sid == event.sid &&
            (event.sequence == sequence || uint32_t(event.sequence - sequence) >= 0x80000000u))
            return;
        sid = event.sid;
        sequence = event.sequence;
        sequenced = true;
        const auto now = Clock::now();
        for (const auto &field : fields) eventTimes[field.first] = now;
        report("GENA", fields);
    }
};
} // namespace bridge
