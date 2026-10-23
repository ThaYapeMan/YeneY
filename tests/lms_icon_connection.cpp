// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/player.h"
#include <atomic>
#include <csignal>
#include <iostream>
static std::atomic_bool stop{false};
static void halt(int) { stop = true; }
struct Sink : yeney::Sink {
    uint64_t frames = 0;
    void trackBoundary(uint64_t, const yeney::Format &, bool) override {}
    size_t write(const yeney::Frame *, size_t n) override {
        frames += n;
        return n;
    }
    void pause() override {}
    void resume() override {}
    void stop() override {}
    void flush() override {}
    uint64_t audibleFrames() const override { return frames; }
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override {}
    bool paced() const override { return false; }
};
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    std::signal(SIGTERM, halt);
    yeney::Config c;
    c.server = "127.0.0.1";
    c.port = std::stoul(argv[1]);
    c.connectionReady = [](const auto &host) { std::cout << "READY " << host << std::endl; };
    Sink sink;
    yeney::Player player(c, sink);
    player.run(stop);
}
