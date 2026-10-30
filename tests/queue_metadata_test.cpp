// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "upnp/queue_metadata.h"
#include <cassert>
#include <iostream>
#include <map>
int main() {
    std::map<unsigned, std::string> reference;
    for (const char *setting : {static_cast<const char *>(nullptr), "artist-title", "title"}) {
        if (setting) setenv("YENEY_TITLE_FORMAT", setting, 1); else unsetenv("YENEY_TITLE_FORMAT");
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
            auto text = upnp::queueDidl(m);
            if (!reference.count(rate)) reference[rate] = text;
            assert(reference[rate] == text);
            upnp::XmlNode root;
            assert(upnp::parseXml(text, root));
            auto item = root.child("item");
            assert(item);
            assert(item->value("title") == m.title);
            assert(item->value("creator") == m.artist);
            assert(item->value("albumArtURI") == m.artwork);
            assert(item->value("artist") == m.artist && item->value("album") == m.album);
            assert(item->value("class") == "object.item.audioItem.musicTrack");
            auto res = item->child("res");
            assert(res && res->text == m.url);
            assert(res->attribute("protocolInfo") == "http-get:*:audio/wav:*");
            assert(res->attribute("duration") == "0:01:05");
            assert(res->attribute("sampleFrequency") == std::to_string(rate));
            assert(res->attribute("size") == std::to_string(m.size));
            assert(item->attribute("id") == "yeney:test");
            auto precise=m;
            precise.frames+=rate/2;precise.size=44+precise.frames*6;precise.preciseDuration=true;
            assert(upnp::queueDidl(precise).find("duration=\"0:01:05.500000000\"")!=std::string::npos);
            m.title = "\x16" "é ü 😀 & <live>";
            assert(upnp::parseXml(upnp::queueDidl(m), root));
            assert(root.child("item")->value("title") == "é ü 😀 & <live>");
            m.title = std::string("Bad") + char(255);
            assert(upnp::parseXml(upnp::queueDidl(m), root));
            assert(root.child("item")->value("title") == "Bad\xef\xbf\xbd");
            // Empty fields must be present, not merely read back as empty.
            for (unsigned mask = 0; mask < 8; ++mask) {
                auto empty = m;
                if (mask & 1) empty.title.clear();
                if (mask & 2) empty.artist.clear();
                if (mask & 4) empty.album.clear();
                upnp::XmlNode parsed;
                assert(upnp::parseXml(upnp::queueDidl(empty), parsed));
                const auto fields = parsed.child("item");
                for (const char *field : {"title", "creator", "artist", "album"})
                    assert(fields->child(field));
            }
            m.title.clear();
            assert(upnp::parseXml(upnp::queueDidl(m), root));
            assert(root.child("item")->value("title").empty());
        }
    }
    std::cout << "PASS queue DIDL element presence: title/creator/artist/album retained for every empty-field combination\n";
    std::cout << "PASS queue plain title: unset/artist-title/title identical, empty stays empty, controls/invalid UTF-8 repaired, Unicode retained\n";
    std::cout << "PASS queue DIDL: escaped title/artist/album, session ownership, native rates, exact WAV "
                 "size and musicTrack protocol\n";
}
