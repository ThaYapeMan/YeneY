// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "volume_control.h"
#include <cassert>
#include <cstdio>
#include <vector>
int main(int argc, char **argv) {
    if (argc > 1) { bridge::volumeSettings("Study", argc > 2 ? argv[2] : nullptr, argv[1]); return 0; }
    using bridge::VolumeSync;
    VolumeSync sync(70);
    std::vector<std::pair<bool,int>> writes;
    std::vector<std::string> commands;
    int caps=0;
    auto send=[&](bool m,int v){writes.emplace_back(m,v);return true;};
    auto cli=[&](const auto &s){commands.push_back(s);};
    auto cap=[&]{++caps;};
    sync.observeLms(25,0,1000);sync.observeSpeaker(20,0,1000,cli);
    sync.write(1199,send,cap);assert(writes.empty());sync.write(1200,send,cap);
    assert(writes.size()==1 && writes.back().second==25 && commands.empty());
    sync.observeSpeaker(25,0,1300,cli);sync.observeLms(25,0,1300);sync.write(1500,send,cap);
    assert(writes.size()==1 && commands.empty());
    sync.observeLms(50,0,1600);sync.observeLms(100,0,1700);
    sync.write(1800,send,cap);assert(writes.size()==1);
    sync.write(1900,send,cap);assert(writes.back().second==70 && caps==1);
    sync.observeSpeaker(70,0,1950,cli);
    sync.observeLms(99,1,2000);sync.write(2200,send,cap);
    assert(writes.back()==std::make_pair(true,1) && caps==1);
    sync.observeSpeaker(70,1,2250,cli);
    sync.observeSpeaker(42,0,2300,cli);
    assert((commands==std::vector<std::string>{"mixer volume 42","mixer muting 0"}));
    sync.observeLms(42,0,2300);sync.observeSpeaker(42,0,2300,cli);sync.write(2600,send,cap);
    assert(writes.size()==3 && commands.size()==2);
    sync.observeLms(60,0,2700);sync.observeSpeaker(43,0,2750,cli);
    sync.observeLms(43,0,2800);sync.write(3100,send,cap);
    assert(writes.size()==3 && commands.back()=="mixer volume 43");
    VolumeSync uncertain(100);
    int attempts=0;
    uncertain.observeLms(50,0,1000);uncertain.observeSpeaker(20,0,1000,cli);
    uncertain.write(1200,[&](bool,int){++attempts;return false;},cap);
    const auto count=commands.size();
    uncertain.observeSpeaker(50,0,1300,cli);uncertain.write(1600,send,cap);
    assert(attempts==1 && commands.size()==count && writes.size()==3);
    VolumeSync group(100);int preparation=0;
    group.observeLms(50,0,1000);group.observeSpeaker(20,0,1000,cli);
    auto prepare=[&]{return ++preparation==1;};
    group.write(1200,send,cap,prepare);assert(writes.size()==3);
    group.write(1399,send,cap,prepare);assert(writes.size()==3);
    group.write(1400,send,cap,prepare);assert(writes.size()==4 && writes.back().second==50);
    VolumeSync slow(100);slow.observeLms(50,0,1000);slow.observeSpeaker(20,0,1000,cli);
    slow.write(1200,send,cap);const auto beforeSlow=commands.size();
    slow.observeSpeaker(30,0,1300,cli);slow.observeSpeaker(40,0,1400,cli);
    slow.observeSpeaker(50,0,1500,cli);assert(commands.size()==beforeSlow);
    slow.observeSpeaker(42,0,1600,cli);assert(commands.back()=="mixer volume 42");
    VolumeSync timeout(100);timeout.observeLms(50,0,1000);timeout.observeSpeaker(20,0,1000,cli);
    timeout.write(1200,send,cap);const auto beforeTimeout=commands.size();
    timeout.observeSpeaker(40,0,1300,cli);assert(commands.size()==beforeTimeout);
    timeout.observeSpeaker(40,0,3300,cli);timeout.observeSpeaker(40,0,3400,cli);
    assert(commands.size()==beforeTimeout+1 && commands.back()=="mixer volume 40");
    puts("PASS volume state machine: trailing debounce, exact values, unchanged suppression, cap once, mute, group snapshots, delayed confirmations, bidirectional echo suppression");
}
