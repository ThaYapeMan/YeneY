// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "third_party/yeney-core/core/decoder.h"
namespace bridge {
// Bind native encoded input to the admitted library identity. The player's
// shared LMS stream remains responsible for Slimproto flow and status.
std::unique_ptr<yeney::Decoder> nativeQueueDecoder(const yeney::DecoderConfig &,
    const std::string &host, const std::string &track, unsigned offsetMs,
    std::function<void(const std::string &)> log = {}, unsigned port = 9000);
}
