// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "stream_metrics.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <string>
static bool workerExit = false;
int main(int argc, char **) {
    workerExit = argc > 1;
    // Register before constructing diagnostics, as HTTP server shutdown does.
    std::atexit([] {
        assert(bridge::streamMetrics().sample().valid);
        std::cout << "PASS stream diagnostics lifetime: " << (workerExit ? "worker-triggered" : "normal")
                  << " exit keeps earlier atexit readers valid (AddressSanitizer)\n";
    });
    std::string heap(2048,'x');
    bridge::streamMetrics().encoder([heap] { return bridge::StreamMeasurement{1,0,heap[100]=='x'}; });
    if (workerExit) {
        std::thread worker([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::exit(0);
        });
        for (;;) {
            assert(bridge::streamMetrics().sample().valid);
            std::this_thread::yield();
        }
    }
}
