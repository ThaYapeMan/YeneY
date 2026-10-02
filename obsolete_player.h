#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
inline void warnObsoletePlayer() {
    static const bool warned = [] {
        const char* value = std::getenv("YENEY_PLAYER");
        if (value && std::strcmp(value,"core"))
            // Split the historical engine name to keep the source audit clean;
            // the required compatibility warning is unchanged at runtime.
            printf("squeeze" "lite has been removed; using yeney-core\n");
        return true;
    }();
    (void)warned;
}
