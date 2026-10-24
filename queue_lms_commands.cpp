// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::speakerCommand(const std::string &command, const std::string &reason) {
    const bool allowed =
        (command == "stop" && (reason == "speaker-stop" || reason == "speaker-source-change")) ||
        (command == "pause 1" && reason == "speaker-pause") ||
        (command == "pause 0" && reason == "speaker-resume");
    if (!allowed)
        throw std::logic_error("invalid speaker command");
    if (command == "pause 0" && current == queue.latestOccurrence() && queue.queueItemProduced(current)) {
        try {
            resumeEchoUntil = 0;
            resumeSnapshot = reader.read(host, mac);
            resumeEchoUntil = milliseconds() + 3000;
        } catch (const std::exception &e) {
            logQueue("resume echo snapshot unavailable: " + std::string(e.what()));
        }
    }
    logQueue("LMS mutation reason=" + reason + " command=" + command);
    if (!queue_audio::controlLms(host, mac, command)) {
        std::string reply;
        const bool verified = queue_audio::controlLms(host, mac, "mode ?", 9090, &reply, 3000);
        logQueue("LMS mutation uncertain command=" + command +
                 " verification=" + (verified ? queue_audio::decodeLms(reply) : "unavailable"));
    }
}
} // namespace bridge
