// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_metadata.h"
#include "../wav_writer.h"
#include <cstdio>
#include <stdexcept>
namespace upnp {
std::string queueDidl(const QueueMetadata &m) {
    using namespace queue_audio;
    const auto suffix = m.url.find_last_of("/.");
    const auto extension = suffix != std::string::npos && m.url[suffix] == '.' ? m.url.substr(suffix) : "";
    if (!extension.empty() && extension != ".wav" && extension != ".flac")
        throw std::invalid_argument("invalid finite queue format");
    const auto format = extension == ".flac" ? Format::Flac : Format::Wav;
    if (!m.rate || m.url.compare(0, 7, "http://") || m.session.empty() ||
        (m.bits != 24 && (format != Format::Flac || m.bits != 16)) || m.channels != 2 ||
        (!m.deferred && m.size != rowLength(format, m.frames, m.rate, m.bits)))
        throw std::invalid_argument("invalid finite queue metadata");
    auto seconds = m.playlistDuration.value_or(m.frames / m.rate);
    char duration[64];
    snprintf(duration, sizeof(duration), "%llu:%02llu:%02llu", (unsigned long long)(seconds / 3600),
             (unsigned long long)(seconds / 60 % 60), (unsigned long long)(seconds % 60));
    std::string declaredDuration(duration);
    if (!m.playlistDuration && m.preciseDuration && m.frames % m.rate) {
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
           "</dc:title>" +
           "<dc:creator>" + esc(m.artist) + "</dc:creator><upnp:artist>" + esc(m.artist) + "</upnp:artist>" +
           "<upnp:album>" + esc(m.album) + "</upnp:album>" +
           (m.artwork.empty() ? std::string{}
                              : "<upnp:albumArtURI>" + esc(m.artwork) + "</upnp:albumArtURI>") +
           "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
           "<res protocolInfo=\"http-get:*:audio/" + formatName(format) + ":*\" duration=\"" +
           declaredDuration + "\"" + (m.deferred ? std::string{} : " size=\"" + std::to_string(m.size) + "\"") + (m.deferred ? std::string{} : " sampleFrequency=\"" + std::to_string(m.rate) + "\"") + " bitsPerSample=\"" + std::to_string(m.bits) + "\" nrAudioChannels=\"2\">" + esc(m.url) +
           "</res></item></DIDL-Lite>";
}
} // namespace upnp
