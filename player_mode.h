#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
enum class PlayerMode { Squeezelite, Core };
inline PlayerMode playerMode() {
    static const auto mode = [] {
        const char* value = std::getenv("YENEY_PLAYER");
        const bool core = value && !std::strcmp(value, "core");
        if (value && !core && std::strcmp(value, "squeezelite"))
            printf("Warning: invalid YENEY_PLAYER='%s'; using squeezelite\n", value);
        printf("YENEY_PLAYER=%s\n", core ? "core" : "squeezelite");
        return core ? PlayerMode::Core : PlayerMode::Squeezelite;
    }();
    return mode;
}
