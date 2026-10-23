// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "lms_artwork.h"
namespace bridge {
// Missing/ambiguous metadata uses the continuous path; finite publication is
// safe only for an explicitly local URL. Inputs have been CLI-decoded once.
inline bool remotePlaybackSource(const std::string &id, const std::string &remote,
                                 const std::string &url) {
    return remoteArtworkTrack(id, remote, url) || url.compare(0, 7, "file://") != 0;
}
} // namespace bridge
