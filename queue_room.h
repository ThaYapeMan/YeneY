// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "playback_mode.h"
#include "room_status.h"
#include "third_party/yeney-core/core/player.h"
#include "upnp/own_speaker_control.h"
namespace bridge {
class QueueRoom {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    QueueRoom(upnp::Speaker, std::shared_ptr<upnp::StreamServer>, std::shared_ptr<upnp::OwnSpeakerControl>,
              PlaybackSelection, std::string server, std::string mac, const uint8_t *,
              std::function<std::string()> streamUri);
    ~QueueRoom();
    void shutdown();
    yeney::Sink &sink();
    void command(const yeney::Command &);
    void refusePlayback(bool member);
    bool queueActive() const;
    int streamMetadataIndex() const;
    bool remoteSource(uint64_t occurrence) const;
    bool deferStart();
    std::optional<yeney::FlacSourceInfo> flacSourceInfo(uint64_t, std::optional<yeney::FlacSourceInfo>);
};
void runQueueCoreClient(const char *, const uint8_t *, const char *, QueueRoom &);
} // namespace bridge
