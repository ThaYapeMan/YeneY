#include "test_temp.h"
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "stream_metrics.h"
#include <cassert>
#include <fstream>
#include <iostream>
int main() {
    bridge::StreamMetrics m;
    m.configure("fixture", testTempRoot());
    m.encoder([] { return bridge::StreamMeasurement{2000, 1000, true}; });
    m.overwrites += 3;
    m.socketWait(10); m.socketWait(2000);
    assert(m.socketBlockedCalls == 1 && m.socketBlockedUs == 2000);
    assert(m.tick(60000)); assert(!m.tick(61000)); assert(!m.tick(119999)); assert(m.tick(120000));
    std::string path = testTempRoot()+"/stream-pacing-" + bridge::StreamMetrics::hex("fixture") + ".json";
    std::ifstream f(path); std::string line; std::getline(f,line);
    assert(line.find(bridge::streamPacingSonos() ? "\"mode\":\"sonos\"" : "\"mode\":\"wall\"") != std::string::npos);
    assert(line.find("\"overwrites\":3") != std::string::npos);
    assert(line.find("\"lead_ms\":1000.000") != std::string::npos);
    bridge::RoomStatus room;
    bridge::ModeStatus playback;
    playback.requested=playback.active="queue";
    playback.path="relay";playback.reason="local-source";
    room.publish(playback);bridge::setRoomStatus(&room);
    m.socketWait(5000000);
    assert(m.socketBlockedCalls==1 && m.socketBlockedUs==2000);
    assert(!m.sample().valid && !m.tick(180000));
    fflush(stdout);
    FILE *capture=tmpfile();assert(capture);
    int saved=dup(STDOUT_FILENO);assert(saved>=0);
    assert(dup2(fileno(capture),STDOUT_FILENO)>=0);
    m.print({2000,1000,true});fflush(stdout);
    assert(ftell(capture)==0);
    assert(dup2(saved,STDOUT_FILENO)>=0);close(saved);fclose(capture);
    playback.path="stream";playback.reason="remote-source";room.publish(playback);
    m.socketWait(3000);
    assert(m.socketBlockedCalls==2 && m.socketBlockedUs==5000);
    assert(m.sample().valid && m.tick(240000));
    bridge::setRoomStatus(nullptr);
    std::cout << "PASS queue path metrics: relay suppresses direct/minute pacing logs and socket accumulation; stream restores unchanged provider/counters/logging\n";
    unlink(path.c_str());
    std::cout << "PASS stream metrics: cumulative overwrite/socket counters, lead snapshot, atomic JSON, minute rate limit\n";
}
