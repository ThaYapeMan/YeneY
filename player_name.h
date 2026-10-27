// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
namespace bridge {
struct PlayerName {
    std::string trueName, displayName, source, mac;
};
// Diagnostic workers may still sample during process teardown.
inline PlayerName &playerName() { static auto *value = new PlayerName; return *value; }
inline PlayerName playerNames(const std::string &room, const char *option, const char *path) {
    std::string configured, line;
    bool found = false;
    std::ifstream input(path);
    while (std::getline(input, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            auto first = s.find_first_not_of(" \t\r");
            return first == std::string::npos ? std::string{} : s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
        };
        if (!found && trim(line.substr(0, eq)) == "YENEY_SHOW_SONOS_SUFFIX") {
            configured = trim(line.substr(eq + 1)); found = true;
        }
    }
    const char *env = std::getenv("YENEY_SHOW_SONOS_SUFFIX");
    const std::string value = option ? option : env ? env : found ? configured : "yes";
    PlayerName result{room + " (Sonos)", room, option ? "command-line" : env ? "environment" : found ? "config" : "default", ""};
    if (value != "yes" && value != "no") printf("yeney: invalid YENEY_SHOW_SONOS_SUFFIX; using yes\n");
    if (value != "no") result.displayName = result.trueName;
    printf("yeney: player name true=%s display=%s source=%s\n", result.trueName.c_str(), result.displayName.c_str(), result.source.c_str());
    return result;
}
} // namespace bridge
