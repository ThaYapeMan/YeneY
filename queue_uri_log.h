// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <string>
// Diagnostic representation only; never use this value for HTTP or ownership.
inline std::string queueUriLog(std::string text) {
    size_t at = 0;
    while ((at = text.find("/yeney/q/", at)) != std::string::npos) {
        at += 9;
        const auto end = text.find_first_of("/\"<> \t\r\n", at);
        if (end == std::string::npos) { text.replace(at, text.size()-at, "..."); break; }
        text.replace(at, end-at, "...");
        at += 3;
    }
    // Queue DIDL also repeats the session in its yeney:<session> item id.
    at = 0;
    while ((at = text.find("yeney:", at)) != std::string::npos) {
        at += 6;
        const auto end = text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_", at);
        const auto count = end == std::string::npos ? text.size()-at : end-at;
        if (count) text.replace(at, count, "...");
        at += 3;
    }
    return text;
}
