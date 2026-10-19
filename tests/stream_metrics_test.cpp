// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "stream_metrics.h"
#include <cassert>
#include <fstream>
#include <iostream>
int main() {
    bridge::StreamMetrics m;
    m.configure("fixture", "/tmp");
    m.encoder([] { return bridge::StreamMeasurement{2000, 1000, true}; });
    m.overwrites += 3;
    m.socketWait(10); m.socketWait(2000);
    assert(m.socketBlockedCalls == 1 && m.socketBlockedUs == 2000);
    assert(m.tick(60000)); assert(!m.tick(61000)); assert(!m.tick(119999)); assert(m.tick(120000));
    std::string path = "/tmp/stream-pacing-" + bridge::StreamMetrics::hex("fixture") + ".json";
    std::ifstream f(path); std::string line; std::getline(f,line);
    assert(line.find(bridge::streamPacingSonos() ? "\"mode\":\"sonos\"" : "\"mode\":\"wall\"") != std::string::npos);
    assert(line.find("\"overwrites\":3") != std::string::npos);
    assert(line.find("\"lead_ms\":1000.000") != std::string::npos);
    unlink(path.c_str());
    std::cout << "PASS stream metrics: cumulative overwrite/socket counters, lead snapshot, atomic JSON, minute rate limit\n";
}
