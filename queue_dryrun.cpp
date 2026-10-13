// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include "queue_report.h"
#include "third_party/yeney-core/core/player.h"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
namespace {
std::atomic_bool stopped{false};
extern "C" void stopSignal(int) { stopped = true; }
} // namespace
int main(int argc, char **argv) {
    std::string server;
    unsigned seconds = 900;
    bool keep = false, transcoded = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") {
            std::cout << "yeney-queue-dryrun --server <ip> [--seconds 900] "
                         "[--keep] [--transcoded]\nOwner-run only; select Queue Test (YeneY) in "
                         "LMS. No Sonos or room-service access.\n";
            return 0;
        }
        if (a == "--transcoded")
            transcoded = true;
        else if (a == "--keep")
            keep = true;
        else if (a == "--server" && i + 1 < argc)
            server = argv[++i];
        else if (a.rfind("--server=", 0) == 0)
            server = a.substr(9);
        else if (a == "--seconds" && i + 1 < argc) {
            try {
                std::string value = argv[++i];
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    return 2;
                auto n = std::stoull(value);
                if (!n || n > UINT32_MAX)
                    return 2;
                seconds = unsigned(n);
            } catch (...) {
                return 2;
            }
        } else {
            std::cerr << "Invalid option: " << a << '\n';
            return 2;
        }
    }
    if (server.empty() || server.find(':') != std::string::npos || !seconds) {
        std::cerr << "--server <IPv4 address or hostname> is required; never "
                     "append :9000\n";
        return 2;
    }
    int lock = open("/tmp/yeney-queue-dryrun.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        std::cerr << "Another dry-run owns the fixed test player\n";
        return 1;
    }
    auto time = std::time(nullptr);
    std::tm local{};
    localtime_r(&time, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::string base = std::string("/tmp/yeney-queue-dryrun-") + stamp + "-" + std::to_string(getpid());
    std::ofstream log(base + ".log");
    bool storeOpened = false;
    try {
        queue_audio::Config storage;
        storage.root = "/tmp/yeney-queue-dryrun";
        queue_audio::CoreQueueSink sink("QueueTest", storage);
        storeOpened = true;
        yeney::Config cfg;
        cfg.server = server;
        cfg.sourceIsTranscoded = [transcoded](uint64_t) { return transcoded; };
        cfg.name = "Queue Test (YeneY)";
        cfg.mac = {0x02, 0x59, 0x51, 0x54, 0, 1};
        cfg.log = [&](const std::string &s) { log << s << std::endl; };
        yeney::Player player(cfg, sink);
        std::signal(SIGINT, stopSignal);
        std::signal(SIGTERM, stopSignal);
        std::exception_ptr playerError;
        std::thread core([&] {
            try {
                player.run(stopped);
            } catch (...) {
                playerError = std::current_exception();
                stopped = true;
            }
        });
        struct JoinOnExit {
            std::thread &thread;
            ~JoinOnExit() {
                stopped = true;
                if (thread.joinable())
                    thread.join();
            }
        } joinOnExit{core};
        std::cout << "Play an album to Queue Test (YeneY), first crossfade off, "
                     "then on.\nReport: "
                  << base << ".md\n";
        using Clock = std::chrono::steady_clock;
        auto start = Clock::now(), last = start;
        double progress = 0;
        uint64_t active = 0;
        std::vector<uint64_t> completed;
        while (!stopped && Clock::now() - start < std::chrono::seconds(seconds)) {
            auto now = Clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            auto rows = sink.snapshot();
            std::sort(rows.begin(), rows.end(),
                      [](const auto &a, const auto &b) { return a.first < b.first; });
            for (const auto &r : rows) {
                if (!r.item ||
                    std::find(completed.begin(), completed.end(), r.source.occurrence) != completed.end())
                    continue;
                std::optional<uint64_t> length;
                try {
                    length = r.item->contentLength();
                } catch (...) {
                    // A playlist replacement cancels the old active asset.
                    // Let the new occurrence become the next audible epoch.
                    if (active == r.source.occurrence) {
                        active = 0;
                        progress = 0;
                    }
                    continue;
                }
                if (!length)
                    break;
                if (!active) {
                    active = r.source.occurrence;
                    progress = 0;
                    sink.audibleFrom(active, r.first);
                    dt = 0;
                }
                if (r.source.occurrence != active)
                    continue;
                if (!sink.isPaused())
                    progress += dt * r.source.rate;
                const uint64_t total = (*length - 44) / 6, available = r.item->producedFrames();
                if (progress > available)
                    progress = available; // real underrun, never advance through absent PCM
                auto played = std::min<uint64_t>(uint64_t(progress), total);
                sink.advanceAudible(r.first + played);
                if (played == total) {
                    sink.audibleEnd(active);
                    completed.push_back(active);
                    active = 0;
                    progress = 0;
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Snapshot before Player shutdown flushes pending assets; timestamps remain
        // explicit.
        stopped = true;
        core.join();
        try {
            sink.waitIdle();
        } catch (const std::exception &e) {
            log << "worker fault: " << e.what() << '\n';
        }
        auto rows = sink.snapshot();
        std::ofstream tsv(base + ".tsv"), report(base + ".md");
        auto verdict = queue_audio::writeReadAheadReport(rows, report, tsv);
        std::cout << verdict.line() << "; worst lead " << verdict.worst << " s\n";
        if (playerError)
            std::rethrow_exception(playerError);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        if (!keep && storeOpened) {
            std::error_code ignored;
            std::filesystem::remove_all("/tmp/yeney-queue-dryrun/QueueTest", ignored);
        }
        close(lock);
        return 1;
    }
    if (!keep)
        std::filesystem::remove_all("/tmp/yeney-queue-dryrun/QueueTest");
    close(lock);
    return 0;
}
