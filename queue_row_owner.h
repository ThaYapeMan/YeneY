// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "core_queue_sink.h"
#include <algorithm>
#include <atomic>
#include <map>

namespace queue_audio {

class QueueRowOwner {
public:
    struct Admission {
        uint64_t token;
        std::string sourceUrl;
        uint64_t request;
        uint64_t cancelledThrough;
        uint64_t firstOccurrence;
    };

    // Request changes are issued under the room command mutex.
    uint64_t nextRequest() { return ++request; }
    uint64_t currentRequest() const { return request.load(); }

    void cancelSourceThrough(uint64_t occurrence) { cancelledThrough = occurrence; }

    bool sourceFaultSuperseded(uint64_t occurrence) const {
        return occurrence <= cancelledThrough;
    }

    // The room worker advances admissions in token order.
    void admit(uint64_t token, const std::string &sourceUrl, uint64_t serial,
               uint64_t firstOccurrence) {
        admissions.insert_or_assign(token, Admission{
            token, sourceUrl, serial, cancelledThrough.load(), firstOccurrence});
    }

    bool drainDeclarations(const std::vector<QueueRecord> &records) {
        for (auto entry = admissions.begin(); entry != admissions.end();) {
            const auto &admission = entry->second;
            const auto declaring = std::find_if(records.begin(), records.end(),
                [&admission](const auto &record) {
                    return record.source.occurrence >= admission.firstOccurrence &&
                           record.sourceToken == admission.token;
                });
            if (declaring == records.end() ||
                (declaring->incomplete.empty() && declaring->fault.empty() &&
                 (!declaring->item || (declaring->pairedFrom &&
                  declaring->actual - declaring->attachOffset < declaring->verificationFrames))))
                return false;
            entry = admissions.erase(entry);
        }
        return true;
    }

    void clearAdmissions() { admissions.clear(); }

private:
    std::atomic<uint64_t> request{0};
    std::atomic<uint64_t> cancelledThrough{0};
    std::map<uint64_t, Admission> admissions;
};

} // namespace queue_audio
