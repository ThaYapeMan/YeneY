// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <functional>
#include <string>
namespace bridge {
void roomLmsCommand(const std::string &host, const std::string &mac,
                           const std::string &command, const std::string &reason,
                           const std::function<void(const std::string &)> &log);
}
