// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace queue_audio {

enum class Format { Wav, Flac, FlacPacked };

inline const char *queueFormatName(Format format) {
    if (format == Format::Wav) return "wav";
    return format == Format::Flac ? "flac-fixed" : "flac-fixed-packed";
}

inline const char *formatName(Format format) {
    return format == Format::Wav ? "wav" : "flac";
}

inline const char *formatExtension(Format format) {
    return format == Format::Wav ? ".wav" : ".flac";
}

inline Format queueFormat(const std::string &room = {}) {
    const auto parse = [](std::string_view value) {
        if (value == "wav") return Format::Wav;
        if (value == "flac-fixed") return Format::Flac;
        if (value != "flac" && value != "flac-fixed-packed")
            std::printf("yeney: queue invalid YENEY_QUEUE_FORMAT; using flac-fixed-packed\n");
        return Format::FlacPacked;
    };
    if (const auto *value = std::getenv("YENEY_QUEUE_FORMAT"))
        return parse(value);
    if (room.empty()) return Format::FlacPacked;

    const auto trim = [](const std::string &value) {
        const auto first = value.find_first_not_of(" \t\r");
        return first == std::string::npos ? std::string{} :
            value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    };
    const auto *config = std::getenv("YENEY_CONFIG");
    std::ifstream input(config ? config : "/etc/yeney/config");
    const auto roomKey = "room." + room + ".YENEY_QUEUE_FORMAT";
    std::optional<std::string> global, local;
    std::string line;
    while (std::getline(input, line)) {
        const auto delimiter = line.find('=');
        if (delimiter == std::string::npos) continue;
        const auto key = trim(line.substr(0, delimiter));
        if (key == roomKey) local = trim(line.substr(delimiter + 1));
        if (key == "YENEY_QUEUE_FORMAT") global = trim(line.substr(delimiter + 1));
    }
    return parse(local.value_or(global.value_or("flac-fixed-packed")));
}

} // namespace queue_audio
