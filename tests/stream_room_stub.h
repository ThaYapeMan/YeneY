// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Extracted stream-control functions run without a QueueRoom in these fixtures.
// Supply only that inactive dependency; routing itself is tested by real yeney.
struct InactiveQueueRoom {
    bool queueActive() const { return false; }
};
static InactiveQueueRoom *gQueueRoom = nullptr;
