// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_metadata.h"
#include <cstdio>
#include <stdexcept>
namespace upnp {
std::string queueDidl(const QueueMetadata &m) {
    if (!m.rate || m.url.compare(0, 7, "http://") || m.session.empty() || m.size < 44 || m.bits != 24 ||
        m.channels != 2 || m.frames > (UINT32_MAX - 36ull) / 6 || m.size != 44 + 6 * m.frames)
        throw std::invalid_argument("invalid finite queue metadata");
    auto seconds = m.frames / m.rate;
    char duration[64];
    snprintf(duration, sizeof(duration), "%llu:%02llu:%02llu", (unsigned long long)(seconds / 3600),
             (unsigned long long)(seconds / 60 % 60), (unsigned long long)(seconds % 60));
    std::string declaredDuration(duration);
    if (m.preciseDuration && m.frames % m.rate) {
        char fraction[32];
        snprintf(fraction, sizeof fraction, ".%09llu",
                 (unsigned long long)((m.frames % m.rate) * 1000000000ull / m.rate));
        declaredDuration += fraction;
    }
    const auto esc = [](const std::string &s) { return xmlEscape(s); };
    return "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
           "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
           "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
           "<item id=\"yeney:" +
           esc(m.session) +
           "\" parentID=\"0\" restricted=\"true\">"
           "<dc:title>" +
           esc(m.title) +
           "</dc:title>"
           "<dc:creator>" +
           esc(m.artist) + "</dc:creator><upnp:artist>" + esc(m.artist) +
           "</upnp:artist>"
           "<upnp:album>" +
           esc(m.album) + "</upnp:album>" +
           (m.artwork.empty() ? std::string{}
                              : "<upnp:albumArtURI>" + esc(m.artwork) + "</upnp:albumArtURI>") +
           "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
           "<res protocolInfo=\"http-get:*:audio/wav:*\" duration=\"" +
           declaredDuration + "\" size=\"" + std::to_string(m.size) + "\" sampleFrequency=\"" +
           std::to_string(m.rate) + "\" bitsPerSample=\"24\" nrAudioChannels=\"2\">" + esc(m.url) +
           "</res></item></DIDL-Lite>";
}
} // namespace upnp
