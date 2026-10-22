// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_coordinator.h"
#include "../playback_mode.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
namespace upnp {
namespace {
// Quote diagnostic fields without truncation or embedded log-line boundaries.
std::string diagnostic(const std::string &text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 32)
            out << "\\u00" << std::hex << std::setfill('0') << std::setw(2) << unsigned(c);
        else
            out << c;
    }
    out << '"';
    return out.str();
}
uint32_t number(const std::string &text) {
    uint64_t value = 0;
    if (text.empty())
        throw std::runtime_error("queue-invalid-number");
    for (char c : text) {
        if (c < '0' || c > '9' || value > (UINT32_MAX - unsigned(c - '0')) / 10)
            throw std::runtime_error("queue-invalid-number");
        value = value * 10 + unsigned(c - '0');
    }
    return uint32_t(value);
}
std::string local(const XmlNode &n) { return n.name.substr(n.name.find(':') + 1); }
uint64_t creation(const XmlNode &item) {
    std::tm tm{};
    std::istringstream input(item.value("date"));
    input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (!input.fail() && input.peek() == 'Z') {
        auto value = timegm(&tm);
        if (value > 0)
            return uint64_t(value);
    }
    auto title = item.value("title");
    if (title.compare(0, 13, "Before YeneY "))
        return 0;
    tm = {};
    tm.tm_isdst = -1;
    std::istringstream name(title.substr(13));
    name >> std::get_time(&tm, "%Y-%m-%d %H:%M");
    if (name.fail())
        return 0;
    auto value = mktime(&tm);
    return value > 0 ? uint64_t(value) : 0;
}
XmlNode didl(const SoapResult &result) {
    XmlNode node;
    if (!parseXml(result.response.value("Result"), node) || local(node) != "DIDL-Lite")
        throw std::runtime_error("queue-invalid-browse-didl");
    return node;
}
} // namespace
QueueCoordinator::QueueCoordinator(Speaker s, std::function<void(const std::string &)> logger, unsigned ms)
    : room(std::move(s)), log(std::move(logger)), timeout(ms) {
    if (bridge::selectPlaybackMode("queue", room, true).selected != bridge::PlaybackMode::Queue || !timeout)
        throw std::invalid_argument("group-rate-undecided");
}
void QueueCoordinator::discover() {
    if (!services.empty())
        return;
    HttpUrl description;
    if (!parseHttpUrl(room.location, description) || description.host != room.ip)
        throw std::runtime_error("queue-device-description-unavailable");
    auto response = httpGet(description, timeout);
    XmlNode root;
    if (response.status != 200 || !response.error.empty() || !parseXml(response.body, root))
        throw std::runtime_error("queue-device-description-invalid");
    const auto device = root.child("device");
    if (!device || device->value("UDN") != "uuid:" + room.uuid)
        throw std::runtime_error("queue-coordinator-identity-changed");
    std::map<std::string, Endpoint> found;
    std::function<void(const XmlNode &)> visit = [&](const XmlNode &node) {
        if (local(node) == "service") {
            const auto urn = node.value("serviceType");
            for (const auto &name : {"AVTransport", "Queue", "ContentDirectory", "ZoneGroupTopology"}) {
                const auto expected = std::string(name) == "Queue"
                                          ? "urn:schemas-sonos-com:service:Queue:1"
                                          : "urn:schemas-upnp-org:service:" + std::string(name) + ":1";
                if (urn != expected)
                    continue;
                auto path = node.value("controlURL");
                HttpUrl url;
                if (!path.empty() && path[0] == '/')
                    url = {description.host, path, description.port};
                else if (path.find("://") == std::string::npos) {
                    auto slash = description.path.rfind('/');
                    url = {description.host, description.path.substr(0, slash + 1) + path, description.port};
                } else if (!parseHttpUrl(path, url))
                    throw std::runtime_error("queue-invalid-control-url");
                if (path.empty() || url.host != description.host || url.port != description.port)
                    throw std::runtime_error("queue-foreign-control-endpoint");
                if (!found.emplace(name, Endpoint{urn, url}).second)
                    throw std::runtime_error("queue-duplicate-service");
            }
        }
        for (const auto &child : node.children)
            visit(child);
    };
    visit(*device);
    if (found.size() != 4)
        throw std::runtime_error("queue-required-service-unavailable");
    services = std::move(found);
}
SoapResult QueueCoordinator::raw(const std::string &service, const std::string &action,
                                 const SoapArguments &args) {
    discover();
    const auto &endpoint = services.at(service);
    std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<s:Envelope "
                       "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:" +
                       action + " xmlns:u=\"" + endpoint.urn + "\">";
    for (const auto &a : args)
        body += "<" + a.first + ">" + xmlEscape(a.second) + "</" + a.first + ">";
    body += "</u:" + action + "></s:Body></s:Envelope>";
    auto http =
        httpPost(endpoint.url,
                 {{"Content-Type", "text/xml"}, {"SOAPACTION", "\"" + endpoint.urn + "#" + action + "\""}},
                 body, timeout);
    auto result = parseSoap(http.body, action);
    const bool poll = action == "GetPositionInfo" || action == "GetTransportInfo";
    if (log && (!poll || !http.error.empty() || http.status != 200 || !result.ok))
        log("queue SOAP action=" + action + " HTTP=" + std::to_string(http.status) +
            " fault=" + result.faultCode +
            ((!http.error.empty() || http.status != 200 || !result.ok)
                 ? " errorCode=" + diagnostic(result.faultCode) + " errorDescription=" +
                       diagnostic(result.faultDescription) + " request=" + diagnostic(body)
                 : ""));
    if (!http.error.empty() || http.status != 200 || !result.ok)
        throw std::runtime_error("queue-soap-error action=" + action + " fault=" + result.faultCode +
                                 " error=" + http.error);
    return result;
}
void QueueCoordinator::fence() {
    auto topology = raw("ZoneGroupTopology", "GetZoneGroupState", {});
    auto speakers = parseTopology(topology.response.value("ZoneGroupState"));
    auto selected =
        std::find_if(speakers.begin(), speakers.end(), [&](const auto &s) { return s.uuid == room.uuid; });
    if (selected == speakers.end() || selected->name != room.name || selected->ip != room.ip ||
        bridge::selectPlaybackMode("queue", *selected, true).selected != bridge::PlaybackMode::Queue)
        throw std::runtime_error("group-rate-undecided");
}
SoapResult QueueCoordinator::transport(const std::string &action, const SoapArguments &args) {
    // Queries do not mutate; all queue/transport writes recheck the lease.
    if (action.compare(0, 3, "Get"))
        fence();
    return raw("AVTransport", action, args);
}
SoapResult QueueCoordinator::releaseTransport(const std::string &action, const SoapArguments &args) {
    if (action != "RemoveTrackRangeFromQueue" && action != "SetCrossfadeMode" && action != "GetCrossfadeMode")
        throw std::logic_error("queue-invalid-release-action");
    return raw("AVTransport", action, args);
}
QueueSnapshot QueueCoordinator::browse() {
    QueueSnapshot snapshot;
    uint32_t total = 0;
    do {
        auto result = raw("Queue", "Browse",
                          {{"QueueID", "0"},
                           {"StartingIndex", std::to_string(snapshot.uris.size())},
                           {"RequestedCount", "128"}});
        auto count = number(result.response.value("NumberReturned"));
        auto nextTotal = number(result.response.value("TotalMatches"));
        auto update = number(result.response.value("UpdateID"));
        if (nextTotal > 60000 ||
            (!snapshot.uris.empty() && (snapshot.update != update || total != nextTotal)))
            throw std::runtime_error("queue-browse-revision-changed");
        total = nextTotal;
        snapshot.update = update;
        auto root = didl(result);
        size_t before = snapshot.uris.size();
        for (const auto &item : root.children) {
            if (local(item) != "item" || !item.child("res"))
                throw std::runtime_error("queue-invalid-entry");
            snapshot.uris.push_back(item.value("res"));
        }
        if (snapshot.uris.size() - before != count || snapshot.uris.size() > total ||
            (!count && before < total))
            throw std::runtime_error("queue-incomplete-browse");
    } while (snapshot.uris.size() < total);
    return snapshot;
}
std::vector<SavedQueue> QueueCoordinator::playlists() {
    std::vector<SavedQueue> saved;
    std::set<std::string> ids;
    uint32_t total = 0, revision = 0;
    do {
        auto result = raw("ContentDirectory", "Browse",
                          {{"ObjectID", "SQ:"},
                           {"BrowseFlag", "BrowseDirectChildren"},
                           {"Filter", "*"},
                           {"StartingIndex", std::to_string(saved.size())},
                           {"RequestedCount", "128"},
                           {"SortCriteria", ""}});
        auto count = number(result.response.value("NumberReturned"));
        auto update = number(result.response.value("UpdateID"));
        auto nextTotal = number(result.response.value("TotalMatches"));
        if (nextTotal > 60000 || (!saved.empty() && (revision != update || nextTotal != total)))
            throw std::runtime_error("queue-playlist-revision-changed");
        total = nextTotal;
        revision = update;
        auto root = didl(result);
        size_t before = saved.size();
        for (const auto &item : root.children) {
            auto id = item.attribute("id");
            auto title = item.value("title");
            if (id.empty() || !ids.insert(id).second)
                throw std::runtime_error("queue-playlist-identity-missing-or-duplicate");
            saved.push_back({id, title, creation(item)});
        }
        if (saved.size() - before != count || saved.size() > total || (!count && before < total))
            throw std::runtime_error("queue-incomplete-playlist-browse");
    } while (saved.size() < total);
    size_t backups = 0;
    bool unknown = false;
    for (const auto &p : saved)
        if (!p.title.compare(0, 13, "Before YeneY ")) {
            ++backups;
            unknown = unknown || !p.created;
        }
    if (backups > 5 && unknown)
        throw std::runtime_error("queue-backup-order-unavailable");
    return saved;
}
void QueueCoordinator::destroyPlaylist(const std::string &id) {
    auto saved = playlists();
    auto item = std::find_if(saved.begin(), saved.end(), [&](const auto &p) { return p.id == id; });
    if (item == saved.end() || item->title.compare(0, 13, "Before YeneY "))
        throw std::runtime_error("queue-backup-identity-changed");
    fence();
    raw("ContentDirectory", "DestroyObject", {{"ObjectID", id}});
}
} // namespace upnp
