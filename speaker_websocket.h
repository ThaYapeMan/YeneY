// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "speaker_position.h"
#include <functional>
#include <memory>
#include <iosfwd>
namespace bridge {
class WebsocketSpeakerPosition : public SpeakerPosition {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    WebsocketSpeakerPosition(std::string host, bool diagnostic = false,
                             std::function<void(const std::string &)> journal = {});
    ~WebsocketSpeakerPosition();
    const char *name() const override { return "websocket"; }
    const char *precision() const override { return "ms"; }
    std::optional<SpeakerPositionSample> position(double now) const override;
    std::string failure() const;
    uint64_t failureCount() const;
    uint64_t revision() const;
    uint64_t diagnosticRevision() const;
    std::string rawState() const;
    std::string quality() const;
    std::string playbackError() const;
    std::string connectionState() const;
    void diagnosticRate(unsigned hz);
    void finishDiagnostic();
    std::string connections() const;
    std::vector<SpeakerPositionSample> takeSamples();
    std::vector<SpeakerPositionSample> takeEvents();
    std::vector<std::string> takeJournal();
    void groupChanged();
};
double positionMonotonic();
int positionSourceCheck(const std::string &host, double seconds, std::ostream &out);
} // namespace bridge
