#pragma once
#include "speaker_uri.h"
#include "stream_session.h"
#include <atomic>
#include <mutex>
#include <string>
// Shared bridge policy for both player engines and HTTP delivery.
inline std::atomic<bool> speakerRelinquished{false};
inline std::mutex sourceMutex;
inline std::string lastSpeakerSource;
inline std::atomic<bool> speakerReclaimPending{false};
