// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void roomLmsCommand(const std::string &host, const std::string &mac,
                           const std::string &command, const std::string &reason,
                           const std::function<void(const std::string &)> &log) {
    const auto volume = command.rfind("mixer volume ", 0) == 0 ? command.substr(13) : std::string{};
    const bool validVolume = !volume.empty() && volume.size() <= 3 &&
        volume.find_first_not_of("0123456789") == std::string::npos && std::stoi(volume) <= 100;
    const bool allowed =
        (reason == "speaker-volume" && (validVolume || command == "mixer muting 0" || command == "mixer muting 1")) ||
        (command == "stop" && (reason == "speaker-stop" || reason == "speaker-source-change" ||
                               reason == "room-fault" || reason == "group-member" || reason == "track-fault-limit")) ||
        (command == "pause 1" && reason == "speaker-pause") ||
        (command == "pause 0" && reason == "speaker-resume");
    if (!allowed) throw std::logic_error("invalid speaker command");
    log("LMS mutation reason=" + reason + " command=" + command);
    if (!queue_audio::controlLms(host, mac, command)) {
        std::string reply;
        const bool verified = queue_audio::controlLms(host, mac, "mode ?", 9090, &reply, 3000);
        log("LMS mutation uncertain command=" + command + " verification=" +
            (verified ? queue_audio::decodeLms(reply) : "unavailable"));
    }
}

void QueueRoom::Impl::speakerCommand(const std::string &command, const std::string &reason) {
    const auto entry = std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.stream == current; });
    const auto source = entry != items.end() && entry->owner ? entry->owner : current;
    if (command == "pause 0" && source == queue.latestOccurrence() && queue.queueItemProduced(source)) {
        try {
            resumeEchoUntil = 0;
            resumeSnapshot = reader.read(host, mac);
            resumeEchoUntil = milliseconds() + 3000;
        } catch (const std::exception &e) {
            logQueue("resume echo snapshot unavailable: " + std::string(e.what()));
        }
    }
    roomLmsCommand(host, mac, command, reason, logQueue);
}
} // namespace bridge
