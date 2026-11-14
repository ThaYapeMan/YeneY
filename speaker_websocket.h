// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "speaker_position.h"
#include <functional>
#include <memory>
namespace bridge {
class WebsocketSpeakerPosition : public SpeakerPosition {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    WebsocketSpeakerPosition(std::string host);
    ~WebsocketSpeakerPosition();
    const char *name() const override { return "websocket"; }
    const char *precision() const override { return "ms"; }
    std::optional<SpeakerPositionSample> position(double now) const override;
    std::string failure() const;
    void groupChanged();
};
double positionMonotonic();
} // namespace bridge
