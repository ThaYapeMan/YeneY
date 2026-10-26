// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdlib>
#include <string>
inline std::string testTempRoot() {
    const char* root = std::getenv("TMPDIR");
    return root && *root ? root : "/var/tmp";
}
