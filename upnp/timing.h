#pragma once
#include <chrono>
namespace upnp { namespace timing {
// Study captures, 27 Sep (2afa221/c35063a): 13/20 Plays after q-Stop
// sent ACTIVE/STANDBY GETs 0.8–6.8 ms apart; none during 32 idle minutes.
// Allow scheduling jitter without treating the ~30 s solitary probe as Play.
constexpr auto kGetPairWindow = std::chrono::milliseconds(25);
// Diagnose a pair without a confirming transport event; never roll it back.
constexpr auto kGetPairConfirmation = std::chrono::seconds(15);
// Preserve the existing noson-compatible transport socket deadline.
constexpr unsigned transportMs = 20000;
// Sonos may withhold SOAP for ~5 s while holding a GET (26 Sep runs).
constexpr unsigned readMs = 5000;
// Existing status thread wakes every half second; events wake it sooner.
constexpr unsigned pollMs = 500;
// Position and fallback media/volume were cached for one second before events-first.
constexpr auto position = std::chrono::seconds(1);
// Existing topology fallback interval, retained when its subscription is missing.
constexpr auto topology = std::chrono::seconds(5);
// Events carry state; a low-frequency read detects a silently stale subscription.
constexpr auto sanity = std::chrono::seconds(30);
// noson requests five-minute subscriptions; renew halfway through the grant.
constexpr unsigned subscriptionSeconds = 300;
// noson retries first after one second, then every five seconds.
constexpr auto retryFirst = std::chrono::seconds(1);
constexpr auto retryLater = std::chrono::seconds(5);
// Bound subscription I/O independently from slow transport actions.
constexpr unsigned subscribeMs = 2000;
// Best-effort shutdown and the initial SID publication race are bounded.
constexpr unsigned unsubscribeMs = 500;
constexpr auto sidRace = std::chrono::milliseconds(500);
// Keep the listener responsive to shutdown and incomplete local NOTIFY requests.
constexpr int acceptMs = 50, receiveMs = 25, notifyMs = 750;
} }
