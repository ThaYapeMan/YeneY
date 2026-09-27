#pragma once
#include "speaker_state.h"
namespace upnp {
inline std::string targetFor(const std::string& action, const SpeakerState& state) {
    const bool transport = action == "SetAVTransportURI" || action == "Play" || action == "Pause"
        || action == "Stop" || action == "GetTransportInfo" || action == "GetPositionInfo" || action == "GetMediaInfo";
    return transport && !state.group.ip.empty() ? state.group.ip : state.room.ip;
}
}
