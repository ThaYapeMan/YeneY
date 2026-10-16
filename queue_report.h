// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "core_queue_sink.h"
#include <ostream>
#include <sstream>
namespace queue_audio {
struct ReadAheadVerdict {
    bool ok = true;
    size_t pairs = 0, incomplete = 0;
    double mismatchedSeconds = 0;
    std::string reasons;
    double worst = -1;
    std::string line() const { return ok ? "LMS read-ahead OK" : "LMS read-ahead INSUFFICIENT"; }
};
inline ReadAheadVerdict writeReadAheadReport(const std::vector<QueueRecord> &rows, std::ostream &report,
                                             std::ostream &tsv) {
    auto count = [](std::optional<uint64_t> n) { return n ? std::to_string(*n) : "unknown"; };
    std::ostringstream table;
    table << "occurrence\trate\texact_frames\tstrm\tdecode_complete\tSTMd\tnext_"
             "strm\taudible_"
             "start\tSTMs\taudible_end\tsuccessor_lead_s\ttransition\toverlap_"
             "frames\tpromised_"
             "frames\tactual_frames\tprefix_"
             "frames\tpublication\tfault\tcodec\tstartup_delay_s"
             "\tpair_status\texpected_lms_index\tlms_poll_stamp\tlms_playlist_"
             "index\tlms_time\tlms_title"
             "\tlms_matches_audible\tmismatched_seconds\n";
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
        bool pendingTail = false;
        for (const auto &tail : rows)
            if (tail.source.occurrence == UINT64_MAX - r.source.occurrence) {
                pendingTail = pendingTail || tail.end < 0;
                if (tail.end >= 0)
                    end = std::max(end, tail.end);
            }
        if (i + 1 < rows.size() && rows[i + 1].prefixFrames) {
            pendingTail = pendingTail || rows[i + 1].audible < 0;
            if (rows[i + 1].audible >= 0)
                end = std::max(end, rows[i + 1].audible);
        }
        if (pendingTail)
            end = -1;
        const double lead = end >= 0 && next >= 0 ? end - next : -1;
        std::string status;
        if (r.source.occurrence >= UINT64_MAX / 2) {
            status = "continuation";
        } else if (end >= 0 && next >= 0) {
            status = "completed";
            if (!v.pairs || lead < v.worst)
                v.worst = lead;
            ++v.pairs;
            if (lead < 12) {
                v.ok = false;
                v.reasons += " lead-below-12s";
            }
        } else {
            ++v.incomplete;
            status = !r.incomplete.empty() ? r.incomplete : !r.fault.empty() ? "source-fault" : "run-end";
        }
        if (r.storageFault) {
            v.ok = false;
            v.reasons += " storage-fault";
        }
        if (r.lmsMismatch) {
            v.ok = false;
            v.reasons += " lms-track-mismatch";
        }
        v.mismatchedSeconds += r.mismatchedSeconds;
        std::ostringstream stamps, indices, times, titles;
        bool unknown = r.lms.empty(), mismatch = false;
        for (size_t j = 0; j < r.lms.size(); ++j) {
            if (j) {
                stamps << '|';
                indices << '|';
                times << '|';
                titles << '|';
            }
            const auto &sample = r.lms[j];
            stamps << sample.stamp;
            indices << sample.index;
            times << sample.time;
            titles << sample.title;
            unknown = unknown || !sample.matches;
            mismatch = mismatch || (sample.matches && !*sample.matches);
        }
        table << r.source.occurrence << '\t' << r.source.rate << '\t' << count(r.source.sourceFrames) << '\t'
              << r.strm << '\t' << r.decoded << '\t' << r.stmd << '\t' << next << '\t' << r.audible << '\t'
              << r.stms << '\t' << end << '\t' << lead << '\t' << r.transition << '\t' << r.overlap << '\t'
              << count(r.promised) << '\t' << r.actual << '\t' << r.prefixFrames << '\t' << r.publication
              << '\t' << r.fault << '\t'
              << (r.source.codec == 'f'   ? "FLAC"
                  : r.source.codec == 'l' ? "ALAC"
                  : r.source.codec == 'm' ? "MP3"
                  : r.source.codec == 'p' ? "PCM"
                  : r.source.codec == 'a' ? "AAC"
                                          : "unknown")
              << '\t' << (r.audible >= 0 && r.strm >= 0 ? r.audible - r.strm : -1) << '\t' << status << '\t'
              << r.expectedIndex << '\t' << stamps.str() << '\t' << indices.str() << '\t' << times.str()
              << '\t' << titles.str() << '\t'
              << (mismatch  ? "false"
                  : unknown ? "unknown"
                            : "true")
              << '\t' << r.mismatchedSeconds << '\n';
    }
    if (!v.pairs) {
        v.ok = false;
        v.reasons += " zero-completed-pairs";
    }
    tsv << table.str();
    report << "# Queue mode phase 2: LMS read-ahead\n\nPlayer: Queue Test "
              "(YeneY), MAC 02:59:51:54:00:01. No "
              "Sonos.\n\nTimes are monotonic seconds from sink creation; -1 "
              "means unobserved, not "
              "zero.\n\n```tsv\n"
           << table.str() << "```\n\nSuccessor pairs: " << v.pairs << "; worst lead: " << v.worst
           << " s. Need >=12 s for every completed pair. Incomplete rows: " << v.incomplete
           << " (run-end, source-fault or flushed).\n\nLMS mismatched seconds: " << v.mismatchedSeconds
           << ". A sustained mismatch >2 s fails with lms-track-mismatch. CLI "
              "values are sampled at 1 Hz; "
              "parallel pipe-separated columns retain every sample. Titles "
              "retain LMS percent encoding.\n\nReasons:"
           << (v.reasons.empty() ? " none" : v.reasons) << "\n\n"
           << v.line() << '\n';
    return v;
}
} // namespace queue_audio
