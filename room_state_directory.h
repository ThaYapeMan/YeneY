// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdlib>
#include <stdexcept>
#include <string>
namespace bridge {
inline std::string roomStateDirectory() {
    const char *directory = std::getenv("STATE_DIRECTORY");
    if (!directory) return "/var/lib/yeney";
    const std::string value(directory);
    if (value.empty() || value.front() != '/' || value.find(':') != std::string::npos)
        throw std::invalid_argument("invalid room STATE_DIRECTORY");
    return value;
}
}
