#include "upnp/soap.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace upnp;
static std::string file(const std::string& path) {
    std::ifstream f(path); assert(f); return {std::istreambuf_iterator<char>(f), {}};
}
int main() {
    setenv("SONOS_LMS_TITLE_FORMAT", "title", 1);
    const std::string title = "T| & < > \" ' café", artist = "A| & < > \" ' Björk", album = "B| & < > \" ' 東京";
    for (auto mode : {StreamContentMode::Structured, StreamContentMode::Plain, StreamContentMode::Off}) {
        const std::string name = mode == StreamContentMode::Structured ? "structured" : mode == StreamContentMode::Plain ? "plain" : "off";
        for (bool empty : {false, true}) {
            const auto didl = streamDidl("http://bridge/stream.flac?x=1&y=2", title, "", empty ? "" : artist, empty ? "" : album, mode);
            assert(didl + "\n" == file("tests/fixtures/stream-content-" + name + (empty ? "-empty" : "") + ".xml"));
            XmlNode xml; assert(parseXml(didl, xml));
            const auto item = xml.child("item"); assert(item);
            if (!empty) assert(item->value("creator") == artist && item->value("album") == album);
            else assert(!item->child("creator") && !item->child("album"));
        }
    }
    for (auto format : {TitleFormat::ArtistTitle, TitleFormat::Title}) {
        const std::string name = format == TitleFormat::ArtistTitle ? "artist-title" : "title";
        for (bool empty : {false, true}) {
            const auto didl = streamDidl("http://bridge/stream.flac?x=1&y=2", title, "",
                empty ? "" : artist, empty ? "" : album, StreamContentMode::Structured, format);
            assert(didl + "\n" == file("tests/fixtures/title-format-" + name + (empty ? "-empty" : "") + ".xml"));
            XmlNode xml; assert(parseXml(didl, xml));
            assert(xml.child("item")->value("title") == formatTitle(title, empty ? "" : artist, format));
            assert(xml.child("item")->value("streamContent") == streamContent(title, empty ? "" : artist,
                empty ? "" : album, StreamContentMode::Structured));
        }
    }
    std::cout << "PASS: golden artist-title/title DIDL, empty artist and XML characters; radio text unchanged\n";
    assert(streamContent("", "", "", StreamContentMode::Structured) == "TYPE=SNG");
    assert(streamContent("", "A|B", "", StreamContentMode::Structured) == "TYPE=SNG|ARTIST A/B");
    assert(streamContent("", "", "", StreamContentMode::Plain).empty());
    assert(streamContent("", "A", "", StreamContentMode::Plain) == "A - ");
    std::cout << "PASS: golden structured/plain/off DIDL, empty artist/album, pipe separators, XML characters and Unicode; creator/album preserved\n";
}
