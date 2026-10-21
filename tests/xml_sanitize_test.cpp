// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "upnp/xml.h"
#include <cassert>
#include <iostream>

int main() {
    upnp::XmlTextChanges changes;
    for (unsigned c = 0; c < 32; ++c) {
        const std::string input(1, char(c));
        const bool allowed = c == 9 || c == 10 || c == 13;
        assert(upnp::xmlSanitize(input, &changes) == (allowed ? input : ""));
        assert(changes.removed == unsigned(!allowed) && changes.replaced == 0);
        assert(upnp::xmlEscape(input) == (allowed ? input : ""));
    }
    for (const auto &input : {"\xef\xbf\xbe", "\xef\xbf\xbf", "\xed\xa0\x80", "\xed\xbf\xbf"}) {
        assert(upnp::xmlSanitize(input, &changes).empty());
        assert(changes.removed == 1 && changes.replaced == 0);
    }
    assert(upnp::xmlEscape("\x16Het beste uit pop in je moerstaal, Vol. 2") ==
           "Het beste uit pop in je moerstaal, Vol. 2");
    std::cout << "PASS XML 1.0 sanitization: field album, every forbidden control, FFFE/FFFF and encoded "
                 "surrogates removed\n";
    const std::string replacement = "\xef\xbf\xbd";
    for (const auto &input : {"\xff", "\xe2\x82", "\xe0\x80\x80", "\xf4\x90\x80\x80"}) {
        assert(upnp::xmlSanitize(input, &changes) == replacement);
        assert(changes.replaced == 1 && changes.removed == 0);
    }
    assert(upnp::xmlSanitize("\xe2(\xa1") == replacement + "(" + replacement);
    assert(upnp::xmlSanitize("\xc0\xaf") == replacement + replacement);
    assert(upnp::xmlEscape("\xff<&") == replacement + "&lt;&amp;");
    std::cout << "PASS XML UTF-8 repair: malformed, truncated, overlong and out-of-range sequences replaced "
                 "without consuming following text\n";
    const std::string valid = u8"é ü 😀\t\n\r \u0080\uD7FF\uE000\uFFFD\U00010000\U0010FFFF";
    assert(upnp::xmlSanitize(valid, &changes) == valid);
    assert(changes.removed == 0 && changes.replaced == 0);
    assert(upnp::xmlEscape(valid) == valid);
    assert(upnp::xmlEscape(valid + " &<>\"'") == valid + " &amp;&lt;&gt;&quot;'");
    std::string title = "\x16Title", album = "\xff";
    const auto summary = upnp::sanitizeXmlFields({{"title", &title}, {"album", &album}});
    assert(title == "Title" && album == replacement);
    assert(summary == "field=title removed=1 replaced=0 field=album removed=0 replaced=1");
    assert(upnp::sanitizeXmlFields({{"title", &title}, {"album", &album}}).empty());
    std::cout << "PASS metadata sanitization summary: one aggregate with field/counts only, sanitized "
                 "display text, no repeated change report\n";
    std::cout << "PASS valid XML text: accented UTF-8, emoji, XML Char boundaries and legacy escaping "
                 "byte-identical\n";
}
