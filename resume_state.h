#ifndef RESUME_STATE_H
#define RESUME_STATE_H

#include <string>
#include <chrono>
#include "upnp/timing.h"

// Caller serializes access. A q-Stop GET pair may observe device Play;
// HTTP requests never change LMS transport intent.
class ResumeState {
public:
    using Clock = std::chrono::steady_clock;
    enum class Unpause { None, NewStream, FeedHeldGet, SameURL };

    Unpause command(char command, bool responseOpen = false) {
        if (command == 's' || command == 'q') clearStoppedPause();
        Unpause action = Unpause::None;
        if (command == 'u') {
            // Decide before clearing the device-resume request. Exactly one
            // action wins, even when a paused seek also has an old held GET.
            action = newStreamPending ? Unpause::NewStream
                : (responseOpen && !requested) ? Unpause::FeedHeldGet
                : Unpause::SameURL;
            newStreamPending = pausedBeforeStop = stoppedResume = false;
        } else if (command == 's') {
            newStreamPending = newStreamPending || paused || pausedBeforeStop;
            pausedBeforeStop = false;
        } else if (command == 'q') {
            // A debounced q before s must not erase the preceding pause.
            pausedBeforeStop = paused || pausedBeforeStop;
            newStreamPending = false;
        }
        if (command == 'p') { paused = true; awaitingPlay = false; expectedPause = true; }
        else if (command == 'u' || command == 's' || command == 'q') {
            paused = false;
            awaitingPlay = command == 'u' || command == 's';
            expectedPause = command == 'q';
            sawPause = transitioning = requested = false;
        }
        return action;
    }

    // Returns true only for a device pause not already commanded by LMS.
    bool observe(const std::string& state) {
        if (state == "STOPPED" && stoppedPauseId) {
            sawPause = true;
            sawStoppedPause = true;
            transitioning = false;
            return false;
        }
        if (state == "PAUSED_PLAYBACK") {
            if (awaitingPlay) return false; // cached pause after an LMS unpause
            bool relay = !sawPause && !paused && !expectedPause;
            sawPause = true;
            transitioning = false;
            return relay;
        }
        if (state == "PLAYING" || state == "TRANSITIONING") {
            // A transport call can delay the diagnostic timer. An event
            // arriving after the window must not erase the missed deadline.
            if (pairConfirmationPending && Clock::now() <= pairConfirmationDeadline)
                pairConfirmationPending = false;
            transitioning = sawPause;
            if (state == "PLAYING") {
                awaitingPlay = false;
                // Keep the observed transition available for takeResume(),
                // including when PLAYING arrives before the fresh GET.
                if (sawStoppedPause) stoppedPauseId = 0;
            }
        }
        else
            transitioning = false;
        return false;
    }

    bool takeResume(unsigned requestedId, unsigned currentId, Clock::time_point now = Clock::now()) {
        if (requestedId != currentId || (!paused && !stoppedResume) || !sawPause || (!transitioning && !getPairObserved) || requested || getPairTaken)
            return false;
        requested = true;
        getPairTaken = getPairObserved;
        resumeDeadline = now + std::chrono::seconds(5);
        return true;
    }

    // Only a successfully completed q-Stop can enable the GET-pair rule.
    void completedQStop(unsigned stream) {
        if (stoppedPauseId == stream && stoppedResume && !paused) pairEligible = true;
    }
    bool observeGetPair(unsigned stream, Clock::duration separation, Clock::time_point now = Clock::now()) {
        if (!pairEligible || !stoppedResume || paused || requested || stream != stoppedPauseId
            || separation < Clock::duration::zero() || separation > upnp::timing::kGetPairWindow)
            return false;
        pairEligible = false;
        getPairObserved = sawPause = true;
        pairConfirmationPending = true;
        pairConfirmationDeadline = now + upnp::timing::kGetPairConfirmation;
        return true;
    }
    bool expireGetPairConfirmation(Clock::time_point now = Clock::now()) {
        if (!pairConfirmationPending || now < pairConfirmationDeadline) return false;
        pairConfirmationPending = false;
        return true;
    }

    bool stopResumeRequested() const { return stoppedResume && requested; }

    void streamStarted() { newStreamPending = false; clearStoppedPause(); }

    void stopForPause(unsigned stream) {
        pairEligible = getPairObserved = getPairTaken = false;
        stoppedPauseId = stream;
        stoppedResume = true;
        sawStoppedPause = sawPause = transitioning = requested = false;
    }
    bool stoppedForPause(unsigned stream) const { return stream && stoppedPauseId == stream; }


    bool expireResume(Clock::time_point now = Clock::now()) {
        if (!requested || now < resumeDeadline) return false;
        requested = transitioning = false;
        return true;
    }

private:
    void clearStoppedPause() {
        if (stoppedPauseId || sawStoppedPause)
            sawPause = transitioning = requested = false;
        pairEligible = getPairObserved = getPairTaken = false;
        stoppedPauseId = 0;
        stoppedResume = false;
        sawStoppedPause = false;
    }
    bool pairEligible = false, getPairObserved = false, getPairTaken = false, pairConfirmationPending = false;
    Clock::time_point pairConfirmationDeadline{};
    unsigned stoppedPauseId = 0;
    bool stoppedResume = false; // completed Stop from p or q permits device resume
    bool sawStoppedPause = false;
    bool newStreamPending = false;
    bool pausedBeforeStop = false;
    bool awaitingPlay = false;
    bool expectedPause = false;
    bool paused = false; // strm p seen, no u/s/q since
    bool sawPause = false;
    bool transitioning = false;
    bool requested = false;
    Clock::time_point resumeDeadline{};
};
#endif
