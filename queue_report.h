// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "core_queue_sink.h"
#include <ostream>
#include <sstream>
namespace queue_audio {
struct ReadAheadVerdict {
    bool ok = true;
    size_t pairs = 0;
    double worst = -1;
    std::string line() const { return ok ? "LMS read-ahead OK" : "LMS read-ahead INSUFFICIENT"; }
};
inline ReadAheadVerdict writeReadAheadReport(const std::vector<QueueRecord> &rows, std::ostream &report,
                                             std::ostream &tsv) {
    auto count = [](std::optional<uint64_t> n) { return n ? std::to_string(*n) : "unknown"; };
    std::ostringstream table;
    table << "occurrence\trate\texact_frames\tstrm\tdecode_complete\tSTMd\tnext_strm\taudible_"
             "start\tSTMs\taudible_end\tsuccessor_lead_s\ttransition\toverlap_frames\tpromised_"
             "frames\tactual_frames\tprefix_frames\tpublication\tfault\n";
    ReadAheadVerdict v;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];
        double next = -1;
        if (r.source.occurrence < UINT64_MAX / 2)
            for (size_t j = i + 1; j < rows.size(); ++j)
                if (rows[j].source.occurrence < UINT64_MAX / 2) {
                    next = rows[j].strm;
                    break;
                }
        double end = r.end;
        for (const auto &tail : rows)
            if (tail.source.occurrence == UINT64_MAX - r.source.occurrence && tail.end >= 0)
                end = std::max(end, tail.end);
        if (i + 1 < rows.size() && rows[i + 1].prefixFrames && rows[i + 1].audible >= 0)
            end = std::max(end, rows[i + 1].audible);
        const double lead = end >= 0 && next >= 0 ? end - next : -1;
        if (next >= 0) {
            if (!v.pairs || lead < v.worst)
                v.worst = lead;
            ++v.pairs;
            if (lead < 12)
                v.ok = false;
        }
        if (!r.fault.empty())
            v.ok = false;
        table << r.source.occurrence << '\t' << r.source.rate << '\t' << count(r.source.sourceFrames) << '\t'
              << r.strm << '\t' << r.decoded << '\t' << r.stmd << '\t' << next << '\t' << r.audible << '\t'
              << r.stms << '\t' << r.end << '\t' << lead << '\t' << r.transition << '\t' << r.overlap << '\t'
              << count(r.promised) << '\t' << r.actual << '\t' << r.prefixFrames << '\t' << r.publication
              << '\t' << r.fault << '\n';
    }
    if (!v.pairs)
        v.ok = false;
    tsv << table.str();
    report << "# Queue mode phase 2: LMS read-ahead\n\nPlayer: Queue Test (YeneY), MAC 02:59:51:54:00:01. No "
              "Sonos.\n\nTimes are monotonic seconds from sink creation; -1 means unobserved, not "
              "zero.\n\n```tsv\n"
           << table.str() << "```\n\nSuccessor pairs: " << v.pairs << "; worst lead: " << v.worst
           << " s. Need >=12 s for every pair; an unobserved audible end is insufficient.\n\n"
           << v.line() << '\n';
    return v;
}
} // namespace queue_audio
