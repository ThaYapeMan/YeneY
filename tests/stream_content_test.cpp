#include "upnp/soap.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace upnp;
static std::string file(const std::string& path) {
    std::ifstream f(path); assert(f); return {std::istreambuf_iterator<char>(f), {}};
}
int main() {
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
    assert(streamContent("", "", "", StreamContentMode::Structured) == "TYPE=SNG");
    assert(streamContent("", "A|B", "", StreamContentMode::Structured) == "TYPE=SNG|ARTIST A/B");
    assert(streamContent("", "", "", StreamContentMode::Plain).empty());
    assert(streamContent("", "A", "", StreamContentMode::Plain) == "A - ");
    std::cout << "PASS: golden structured/plain/off DIDL, empty artist/album, pipe separators, XML characters and Unicode; creator/album preserved\n";
}
