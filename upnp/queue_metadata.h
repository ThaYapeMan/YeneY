// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "soap.h"
#include <cstdint>
namespace upnp {
struct QueueMetadata {
    std::string url, session, title, artist, album, artwork;
    uint64_t frames = 0, size = 0;
    uint32_t rate = 0;
    unsigned bits = 24, channels = 2;
};
// Finite music metadata has no radio URI rewriting or streamContent changes.
std::string queueDidl(const QueueMetadata &, TitleFormat = titleFormat());
} // namespace upnp
