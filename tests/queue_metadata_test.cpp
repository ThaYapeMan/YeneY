// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "upnp/queue_metadata.h"
#include <cassert>
#include <iostream>
int main() {
    for (unsigned rate : {44100, 48000}) {
        upnp::QueueMetadata m;
        m.url = "http://localhost/music/queue.wav?session=test&epoch=1&item=2&revision=3";
        m.session = "test";
        m.title = "A & B <live>";
        m.artist = "Artist > C";
        m.album = "Album & one";
        m.artwork = "http://lms:9000/music/12/cover.jpg";
        m.frames = rate * 65;
        m.rate = rate;
        m.size = 44 + m.frames * 6;
        auto text = upnp::queueDidl(m, upnp::TitleFormat::ArtistTitle);
        upnp::XmlNode root;
        assert(upnp::parseXml(text, root));
        auto item = root.child("item");
        assert(item);
        assert(item->value("title") == m.artist + " - " + m.title);
        assert(item->value("artist") == m.artist && item->value("album") == m.album);
        assert(item->value("class") == "object.item.audioItem.musicTrack");
        auto res = item->child("res");
        assert(res && res->text == m.url);
        assert(res->attribute("protocolInfo") == "http-get:*:audio/wav:*");
        assert(res->attribute("duration") == "0:01:05");
        assert(res->attribute("sampleFrequency") == std::to_string(rate));
        assert(res->attribute("size") == std::to_string(m.size));
        assert(item->attribute("id") == "yeney:test");
    }
    std::cout << "PASS queue DIDL: escaped title/artist/album, session ownership, native rates, exact WAV "
                 "size and musicTrack protocol\n";
}
