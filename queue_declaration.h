// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_item_store.h"
#include "third_party/yeney-core/core/decimator.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace queue_audio {
// Declarations use source sample counts, never an LMS duration rounded to
// milliseconds. The decoder's exactFrames() has already applied this same
// conversion rule before it supplies a TrackDescriptor to the natural sink.
struct RenderedSource {
    uint32_t rate;
    uint64_t frames;
};
inline RenderedSource declaredSource(uint32_t rate, uint64_t frames, uint32_t conversionRate) {
    if (!rate) throw std::invalid_argument("declaration source rate missing");
    const auto rendered = conversionRate ? yeney::Decimator::targetRate(rate, conversionRate) : rate;
    const auto factor = rate / rendered;
    return {rendered, frames / factor + uint64_t(frames % factor != 0)};
}
inline std::optional<uint64_t> declaredLocalFrames(const yeney::TrackDescriptor &source,
                                                   const yeney::TrackDescriptor &next,
                                                   uint64_t prefixFrames) {
    if (!source.sourceFrames) return {};
    uint64_t overlap = 0;
    if (next.transition == 1 && next.rate == source.rate) {
        if (!next.sourceFrames) return {};
        overlap = std::min({next.transitionFrames,
                           *source.sourceFrames ? *source.sourceFrames - 1 : 0, *next.sourceFrames});
    }
    const auto frames = *source.sourceFrames - overlap;
    if (prefixFrames > UINT64_MAX - frames) throw std::overflow_error("local declaration overflow");
    return frames + prefixFrames;
}
inline uint64_t declaredRemoteFrames(double duration, uint32_t rate) {
    if (!rate || !std::isfinite(duration) || duration <= 0)
        throw std::invalid_argument("remote declaration duration missing");
    const auto frames = std::ceil(duration * rate);
    const auto margin = uint64_t(remoteMarginSeconds) * rate;
    if (!std::isfinite(frames) || frames >= double(UINT64_MAX - margin))
        throw std::overflow_error("remote declaration overflow");
    return uint64_t(frames) + margin;
}
} // namespace queue_audio
