// YeneY -- deploy Sonos in a Logitech Media Server (LMS) streaming environment
//
// Copyright (C) 2026 Jaap van Vliet
//
// This file is part of YeneY.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Shared Sonos playback position: polled via UPnP in the main loop,
// consumed by the output thread to correct slimproto position reporting.

#include "sonos-position.h"

#include "position_state.h"
#include "timing_probe_runtime.h"
#include "start_lead.h"
#include <chrono>
#include <mutex>

static std::mutex positionMutex;
static ConnectionPosition position;
static uint64_t positionNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void reset_sonos_position(unsigned stream) {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.reset(stream);
    if (timing_probe::enabled()) timing_probe::diagnostics().reset("stream-reset",true);
}
void sonos_position_connection(unsigned stream, uint64_t request) {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.connection(stream, request, positionNow());
    if (timing_probe::enabled()) timing_probe::diagnostics().reset("reconnect",true);
}
void sonos_position_pcm(unsigned stream, uint64_t request, uint64_t firstFrame) {
    std::lock_guard<std::mutex> lock(positionMutex);
    const auto token=position.token();
    position.pcm(stream, request, firstFrame, positionNow(), yeney_start_lead_ms() != 0);
    if (token!=position.token() && timing_probe::enabled()) timing_probe::diagnostics().anchor(stream,position.measurementBase());
}
uint64_t sonos_position_poll_token(void) {
    std::lock_guard<std::mutex> lock(positionMutex);
    return position.token();
}
void set_sonos_position_ms(uint64_t token, uint32_t ms) {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.poll(token, ms, positionNow());
}
uint64_t get_sonos_position_frames(uint32_t rate) {
    std::lock_guard<std::mutex> lock(positionMutex);
    return position.frames(rate);
}

uint64_t get_sonos_audible_frames(uint32_t rate) {
    std::lock_guard<std::mutex> lock(positionMutex);
    if (timing_probe::enabled() && timing_probe::lmsPositionFromModel()) {
        auto &diagnostic = timing_probe::diagnostics();
        double zeroTime;
        if (position.needsStartEvidence() && diagnostic.playbackConfirmed(position.streamId()) &&
            diagnostic.startupZero(position.streamId(), zeroTime)) position.startZero(zeroTime * 1000);
        uint32_t startupMs; double startupTime;
        if (position.needsStartEvidence() && diagnostic.startupReading(position.streamId(), startupMs, startupTime))
            position.poll(position.token(), startupMs, uint64_t(startupTime * 1000));
        double frame = 0, scale = diagnostic.priorScale(), relSecond = NAN, observed = NAN;
        const bool model = diagnostic.lmsPosition(position.streamId(), position.measurementBase(), rate, frame, scale, &relSecond, &observed);
        const auto audible = position.smoothFrames(rate, timing_probe::clockSeconds() * 1000, model, frame, scale, relSecond, observed);
        diagnostic.reporting(position.phase(diagnostic.priorActive()), position.offset());
        return audible;
    }
    return position.audibleFrames(rate);
}

void sonos_position_allow_zero_start() {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.allowZeroStart();
}

int sonos_position_audio_started() {
    std::lock_guard<std::mutex> lock(positionMutex);
    return position.audioStarted() || (timing_probe::enabled() &&
        (position.needsStartEvidence() ? timing_probe::diagnostics().playbackConfirmed(position.streamId())
                                      : timing_probe::diagnostics().heardAudio(position.streamId())));
}

uint64_t estimate_sonos_audible_frames(uint32_t rate) {
    std::lock_guard<std::mutex> lock(positionMutex);
    auto copy = position;
    if (timing_probe::enabled() && (bridge::streamPacingSonos() || timing_probe::lmsPositionFromModel())) {
        auto &d = timing_probe::diagnostics();
        double frame = 0, scale = d.priorScale(), rel = NAN, at = NAN;
        bool fit = d.lmsPosition(copy.streamId(), copy.measurementBase(), rate, frame, scale, &rel, &at);
        if (bridge::streamPacingSonos() && fit) return uint64_t(frame);
        return copy.smoothFrames(rate, timing_probe::clockSeconds()*1000, fit, frame, scale, rel, at);
    }
    return copy.audibleFrames(rate);
}

int sonos_stream_clock(unsigned stream, uint32_t rate, double *frame, double *scale) {
    std::lock_guard<std::mutex> lock(positionMutex);
    auto &d = timing_probe::diagnostics();
    *scale = d.priorScale();
    *frame = 0;
    double fittedScale = *scale;
    bool locked = position.streamId() == stream &&
        d.lmsPosition(stream, position.measurementBase(), rate, *frame, fittedScale);
    if (locked) *scale = fittedScale;
    return locked;
}
