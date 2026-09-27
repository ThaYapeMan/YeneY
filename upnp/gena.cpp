#include "gena.h"
#include <map>
namespace upnp {
bool parseLastChange(const std::string& body, GenaEvent& event) {
    StateUpdate update;
    if (!parseAvTransport(body, update)) return false;
    event.update = update;
    event.state = update.state.value_or(""); event.status = update.status.value_or("");
    return true;
}
namespace {
bool decimal(const std::string& text, uint64_t& value, uint64_t max) {
    if (text.empty()) return false;
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9' || value > (max - (c - '0')) / 10) return false;
        value = value * 10 + c - '0';
    }
    return value <= max;
}
}
bool parseGenaHeaders(const std::string& path, const std::string& version,
                      const RequestHeaders& fields, size_t& length, GenaEvent& event) {
    if (version != "HTTP/1.1" || (path != "/avt" && path != "/rc" && path != "/zgt")) return false;
    std::map<std::string, std::string> headers;
    for (auto field : fields) {
        for (auto& c : field.first) c = std::tolower(static_cast<unsigned char>(c));
        if (!headers.emplace(field).second) return false;
    }
    uint64_t size = 0, seq = 0;
    if (headers["nt"] != "upnp:event" || headers["nts"] != "upnp:propchange"
        || headers["sid"].empty() || headers.count("transfer-encoding")
        || !decimal(headers["content-length"], size, 1024 * 1024)
        || !decimal(headers["seq"], seq, UINT32_MAX)) return false;
    length = size; event.sid = headers["sid"]; event.sequence = seq;
    event.service = path == "/avt" ? Service::AVTransport : path == "/rc" ? Service::RenderingControl : Service::ZoneGroupTopology;
    return true;
}
bool parseGenaBody(const std::string& body, GenaEvent& event) {
    event.body = body;
    return event.service == Service::AVTransport ? parseLastChange(body, event) :
        event.service == Service::RenderingControl ? parseRenderingControl(body, event.update) :
        parseZoneGroupEvent(body, event.topology);
}
}
