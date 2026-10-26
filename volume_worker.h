// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "volume_control.h"
#include "queue_lms_cli.h"
#include "queue_lms_commands.h"
#include "upnp/own_speaker_control.h"
namespace bridge {
class VolumeWorker {
    std::atomic<bool> stopping{false};
    std::thread worker;
public:
    VolumeWorker(std::shared_ptr<upnp::OwnSpeakerControl> speaker, std::string host,
                 std::string mac, VolumeSettings settings) {
        if (!settings.speaker || host.empty()) return;
        speaker->enableGroupVolume();
        worker = std::thread([this, speaker, host, mac, settings] {
            VolumeSync sync(settings.maximum);
            auto log = [](const std::string &s) { printf("yeney: volume %s\n", s.c_str()); };
            auto number = [](const std::string &s, int low, int high) -> std::optional<int> {
                try { size_t end = 0; const int v = std::stoi(s, &end);
                    if (end == s.size() && v >= low && v <= high) return v;
                } catch (...) {}
                return {};
            };
            auto query = [&](const std::string &command, int low, int high) -> std::optional<int> {
                std::string reply;
                const bool mute = command == "mixer muting";
                if (!queue_audio::controlLms(host, mac, command + " ?", 9090, &reply, 500, &stopping, false, mute)) return {};
                reply = queue_audio::decodeLms(reply);
                while (!reply.empty() && (reply.back() == '\r' || reply.back() == ' ')) reply.pop_back();
                // LMS 9.1.1 leaves the initial false mute preference undefined,
                // returning a validated empty result until it is first set.
                if (mute && reply == mac + " " + command) return 0;
                const auto at = reply.find_last_of(" ");
                return number(reply.substr(at + 1), low, high);
            };
            const auto milliseconds = [] { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()); };
            bool groupPrepared = false;
            int snapshotVolume = -1;
            std::vector<std::string> snapshotMembers;
            std::optional<int> previousMute;
            uint64_t muteChanged = 0;
            std::string identity;
            while (!stopping) {
                const auto now = milliseconds();
                const auto room = speaker->speaker();
                const bool group = room.coordinator == room.name && room.members.size() > 1;
                if (groupPrepared && snapshotMembers != room.members) groupPrepared = false;
                const auto target = room.uuid + (group ? ":group" : ":room");
                if (target != identity) { identity = target; sync.reset(); groupPrepared = false; }
                auto v = query("mixer volume", -100, 100), m = query("mixer muting", 0, 1);
                if (v && m) {
                    if (previousMute != *m) { previousMute = *m; muteChanged = milliseconds(); }
                    sync.observeLms(std::abs(*v), *m, now);
                    upnp::SoapArguments args{{"InstanceID", "0"}};
                    if (!group) args.emplace_back("Channel", "Master");
                    const auto vr = speaker->volumeAction(group ? "GetGroupVolume" : "GetVolume", args, group);
                    const auto mr = speaker->volumeAction(group ? "GetGroupMute" : "GetMute", args, group);
                    const auto sv = number(vr.response.value("CurrentVolume"), 0, 100);
                    const auto sm = number(mr.response.value("CurrentMute"), 0, 1);
                    if (vr.ok && mr.ok && sv && sm) {
                        if (groupPrepared && snapshotVolume != *sv) groupPrepared = false;
                        std::string appVolume, appMute;
                        sync.observeSpeaker(*sv, *sm, milliseconds(), [&](const auto &command) {
                            groupPrepared = false;
                            (command.rfind("mixer volume ", 0) == 0 ? appVolume : appMute) = command;
                        });
                        auto send = [&](const std::string &command) {
                            if (!stopping) roomLmsCommand(host, mac, command, "speaker-volume", log);
                        };
                        auto settle = [&](uint64_t until) {
                            while (!stopping && milliseconds() < until)
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        };
                        if (!appVolume.empty()) {
                            // LMS volume implicitly unmutes. Its mute commands
                            // also finish a 312.5 ms fade with a callback. Let
                            // that callback settle before setting an app level.
                            if (*sm == 0 && (!appMute.empty() || *m != 0)) {
                                send("mixer muting 0"); previousMute = 0;
                                muteChanged = milliseconds(); settle(muteChanged + 400);
                            } else settle(muteChanged + 400);
                            send(appVolume);
                            if (*sm == 1 && (!appMute.empty() || *m == 1)) {
                                send("mixer muting 1"); previousMute = 1; muteChanged = milliseconds();
                            }
                        } else if (!appMute.empty()) {
                            send(appMute); previousMute = *sm; muteChanged = milliseconds();
                        }
                        sync.write(milliseconds(), [&](bool mute, int value) {
                            auto writeArgs = args;
                            writeArgs.emplace_back(mute ? "DesiredMute" : "DesiredVolume", std::to_string(value));
                            groupPrepared = false;
                            return speaker->volumeAction(group ? (mute ? "SetGroupMute" : "SetGroupVolume") :
                                (mute ? "SetMute" : "SetVolume"), writeArgs, group).ok;
                        }, [&] { log("LMS value capped maximum=" + std::to_string(settings.maximum)); }, [&] {
                            if (!group || groupPrepared) return false;
                            groupPrepared = speaker->volumeAction("SnapshotGroupVolume", args, true).ok;
                            snapshotVolume = *sv; snapshotMembers = room.members;
                            return true; // A SOAP write; SetGroupVolume waits another 200 ms.
                        });
                    }
                }
                const auto revision = speaker->volumeRevision();
                for (int i = 0; i < 10 && !stopping && speaker->volumeRevision() == revision; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    ~VolumeWorker() { stopping = true; if (worker.joinable()) worker.join(); }
};
} // namespace bridge
