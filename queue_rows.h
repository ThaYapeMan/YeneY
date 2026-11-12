// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
namespace bridge {
inline unsigned queueRows(const std::string &room) {
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r");
        return first == std::string::npos ? std::string{} : value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    };
    const char *path = std::getenv("YENEY_CONFIG");
    std::ifstream input(path ? path : "/etc/yeney/config");
    std::optional<std::string> global, local;
    std::string line;
    while (std::getline(input, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "YENEY_QUEUE_ROWS" && !global) global = value;
        if (key == "room." + room + ".YENEY_QUEUE_ROWS" && !local) local = value;
    }
    const char *env = std::getenv("YENEY_QUEUE_ROWS");
    const auto text = env ? std::string(env) : local ? *local : global ? *global : "500";
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 5)
        throw std::invalid_argument("invalid YENEY_QUEUE_ROWS");
    const auto count = std::stoul(text);
    if (!count || count > 60000) throw std::invalid_argument("invalid YENEY_QUEUE_ROWS");
    return unsigned(count);
}
} // namespace bridge
