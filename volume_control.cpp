// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "volume_control.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
namespace bridge {
VolumeSettings volumeSettings(const std::string &room, const char *option, const char *path) {
    VolumeSettings result;
    std::optional<std::string> configMode, configMax;
    std::ifstream input(path);
    std::string line;
    const auto prefix = "room." + room + ".";
    while (std::getline(input, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq), value = line.substr(eq + 1);
        auto trim = [](std::string s) {
            const auto first = s.find_first_not_of(" \t\r");
            return first == std::string::npos ? std::string{} : s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
        };
        key = trim(key); value = trim(value);
        if (key == prefix + "lms_volume" && !configMode) configMode = value;
        if (key == prefix + "volume_max" && !configMax) configMax = value;
    }
    const char *env = std::getenv("YENEY_LMS_VOLUME"), *max = std::getenv("YENEY_VOLUME_MAX");
    std::string value = option ? option : env ? env : configMode.value_or("off");
    result.source = option ? "command-line" : env ? "environment" : configMode ? "config" : "default";
    if (value != "off" && value != "speaker")
        printf("yeney: invalid YENEY_LMS_VOLUME; using off\n");
    result.speaker = value == "speaker";
    if (!max && configMax) max = configMax->c_str();
    if (max) {
        std::string text(max);
        int parsed = 0;
        bool valid = !text.empty();
        for (char c : text) {
            if (c < '0' || c > '9' || parsed > 100) { valid = false; break; }
            parsed = parsed * 10 + c - '0';
        }
        if (valid && parsed <= 100) result.maximum = parsed;
        else printf("yeney: invalid YENEY_VOLUME_MAX; using 100\n");
    }
    printf("yeney: LMS volume=%s source=%s maximum=%d\n", result.speaker ? "speaker" : "off", result.source.c_str(), result.maximum);
    return result;
}
} // namespace bridge
