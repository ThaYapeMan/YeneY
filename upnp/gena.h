#pragma once
#include "event_parsers.h"
#include "stream_server.h"
#include <cstdint>
#include <functional>
#include <string>
namespace upnp {
struct GenaEvent {
    std::string sid, state, status;
    uint32_t sequence = 0;
    Service service = Service::AVTransport;
    std::string body;
    StateUpdate update;
    std::vector<Speaker> topology;
};
using GenaHandler = std::function<bool(const GenaEvent&)>;
// Validation is independent of the shared HTTP listener and bridge state.
bool parseLastChange(const std::string&, GenaEvent&);
bool parseGenaHeaders(const std::string& path, const std::string& version,
                      const RequestHeaders&, size_t& length, GenaEvent&);
bool parseGenaBody(const std::string&, GenaEvent&);
}
