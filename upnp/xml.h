#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <vector>
namespace upnp {
struct XmlTextChanges {
    std::size_t removed = 0, replaced = 0;
};
// Preserve valid UTF-8 exactly, drop XML 1.0 forbidden code points, replace
// malformed UTF-8 with U+FFFD. Encoded surrogate code points are dropped.
std::string xmlSanitize(const std::string& text, XmlTextChanges* changes = nullptr);
// Text or double-quoted attributes; apostrophes remain literal, as pinned by the wire fixtures.
std::string xmlEscape(const std::string& text);
bool xmlUnescape(const std::string& text, std::string& out);
struct XmlNode {
    std::string name, text;
    std::map<std::string, std::string> attributes;
    std::vector<XmlNode> children;
    const XmlNode* child(const std::string& localName) const;
    std::string value(const std::string& localName) const;
    std::string attribute(const std::string& key) const;
};
// Small bounded XML reader: namespaces by local name, entities, comments, CDATA.
// Rejects DTDs/external entities, malformed input, excessive nesting and size.
bool parseXml(const std::string& text, XmlNode& root);
}
