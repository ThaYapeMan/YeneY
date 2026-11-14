// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdlib>
#include <fstream>
#include <string>
#include <stdexcept>
namespace bridge {
inline std::string positionSetting(const std::string &room, const char *cli = nullptr) {
    const char *path = std::getenv("YENEY_CONFIG");
    std::ifstream file(path ? path : "/etc/yeney/config");
    std::string global, local, line;
    auto trim = [](std::string s) {
        auto a = s.find_first_not_of(" \t\r");
        return a == std::string::npos ? std::string{} : s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
    };
    while (std::getline(file, line)) {
        auto eq = line.find('='); if (eq == std::string::npos) continue;
        auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "YENEY_POSITION_SOURCE" && global.empty()) global = value;
        if (key == "room." + room + ".YENEY_POSITION_SOURCE" && local.empty()) local = value;
    }
    const auto env = std::getenv("YENEY_POSITION_SOURCE");
    const std::string value = cli ? cli : env ? env : !local.empty() ? local : !global.empty() ? global : "auto";
    if (value != "auto" && value != "upnp" && value != "websocket") throw std::invalid_argument("invalid YENEY_POSITION_SOURCE");
    return value;
}
}
