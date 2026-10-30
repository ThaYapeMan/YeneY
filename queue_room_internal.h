// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "core_queue_sink.h"
#include "lms_icon.h"
#include "queue_path_sink.h"
#include "queue_playlist.h"
#include "queue_protocol.h"
#include "queue_room.h"
#include "queue_lms_commands.h"
#include "queue_route.h"
#include "queue_source.h"
#include "queue_speaker_changes.h"
#include "transport_source.h"
#include "stream_metrics.h"
#include "stream_session.h"
#include "upnp/queue_coordinator.h"
#include "upnp/queue_metadata.h"
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <deque>
#include <set>
#include <thread>
extern "C" void yeney_queue_stream_handoff();
namespace bridge {
namespace queue_room_detail {
extern std::atomic_bool roomStop;
void stopRoom(int);
void logQueue(const std::string &);
unsigned pollSetting();
} // namespace queue_room_detail
using namespace queue_room_detail;
struct TrackFault : std::runtime_error { using std::runtime_error::runtime_error; };
struct QueueRoom::Impl {
    upnp::Speaker room;
    std::string host, mac, session;
    std::function<std::string()> streamUrl;
    std::shared_ptr<upnp::OwnSpeakerControl> speaker;
    queue_audio::CoreQueueSink queue;
    QueuePathSink paths;
    std::atomic_bool selectionDeferred{false};
    std::atomic<int> metadataIndex{-1};
    int selectedIndex = -1;
    bool selecting = false, selectionKnown = false, selectedLocal = true, stoppedPlay = true;
    bool selectedNewPlay = true;
    yeney::Command selectedCommand{'s', 0, false}, requestedCommand{'s', 0, false};
    std::atomic<uint64_t> selectionRequest{0};
    uint64_t selectedRequest = 0;
    queue_audio::PlaylistEntry selectedTrack;
    std::map<uint64_t, queue_audio::PlaylistEntry> sourceTracks;
    std::map<std::string, yeney::FlacSourceInfo> flacCounts;
    std::chrono::steady_clock::time_point handoffAt{};
    std::string pathReason = "local-source";
    queue_audio::Route route;
    upnp::QueueCoordinator coordinator;
    upnp::QueueActions actions;
    std::set<std::string> owned;
    upnp::QueueOwnership ownership;
    RoomStatus status;
    std::shared_ptr<SpeakerChanges> speakerChanges = std::make_shared<SpeakerChanges>();
    std::string publishedState, publishedUri;
    struct Entry {
        uint64_t stream;
        std::string uri;
        std::shared_ptr<queue_audio::Item> item;
        std::string sourceUrl;
        uint64_t frames;
        std::string title;
        uint64_t owner = 0, segmentOffset = 0;
    };
    std::vector<Entry> items;
    std::map<uint64_t, bool> remote;
    std::mutex mutex, sourceMutex;
    std::condition_variable changed;
    std::deque<std::pair<char, uint64_t>> commands;
    std::thread worker;
    bool roomStopped = false;
    std::deque<std::chrono::steady_clock::time_point> trackFaults;
    void roomFault(const std::string &reason, bool member = false);
    bool closing = false, started = false, faulted = false, releasing = false;
    uint64_t firstStream = 1, current = 0;
    queue_audio::PlaylistSnapshot playlist;
    queue_audio::PlaylistReader reader;
    std::string state = "STOPPED", fault, transportStatus;
    std::chrono::steady_clock::time_point playAt{};
    bool waitingForPlay = false;
    bool queueSelectedForAdmission = false;
    std::string takeoverSource = "unknown";
    bool externalReleased = false, stopAcknowledged = false, sourceAway = false;
    std::atomic<int64_t> resumeEchoUntil{0};
    std::atomic_bool resumeFlush{false}, finitePause{false};
    struct PodcastFields { std::string title, artist, album; bool complete = false; };
    std::map<std::string, PodcastFields> plainRemoteTitles;
    std::chrono::steady_clock::time_point deferredStop{};
    uint64_t deferredNext = 0;
    queue_audio::PlaylistSnapshot resumeSnapshot;
    bool pausedFlushRetained = false, remoteNaturalSuccessor = false;
    static int64_t milliseconds();
    std::string expectedState = "STOPPED";
    std::chrono::steady_clock::time_point ownUntil{};
    uint64_t lastElapsed = 0;
    // The only outgoing CLI mutation path: called solely from speaker evidence.
    void speakerCommand(const std::string &command, const std::string &reason);
    void externalStop(const std::string &reason);
    unsigned pollMs = pollSetting();
    std::chrono::steady_clock::time_point nextPoll{}, fastUntil{};
    std::shared_ptr<std::atomic_bool> eventDirty = std::make_shared<std::atomic_bool>(false);
    void fastPoll();
    upnp::QueueOwnershipIO ownershipIO();
    Impl(upnp::Speaker r, std::shared_ptr<upnp::StreamServer> http,
         std::shared_ptr<upnp::OwnSpeakerControl> s, std::string h, std::string m, const uint8_t *rawMac,
         std::function<std::string()> url);
    std::optional<yeney::FlacSourceInfo> sourceInfo(uint64_t occurrence,
                                                    std::optional<yeney::FlacSourceInfo> info);
    void relayEvents(bool enabled);
    void received(const yeney::Command &c);
    void beginSelection(uint64_t request);
    void selectPath();
    void handoffComplete();
    void publish();
    void release(bool stop = false);
    void command(char c, uint64_t next);
    void append();
    void observe();
    void run();
    void shutdown();
    ~Impl();
};
} // namespace bridge
