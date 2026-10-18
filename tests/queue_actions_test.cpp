// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "upnp/queue_actions.h"
#include <cassert>
#include <iostream>
#include <map>
int main() {
    std::string action;
    std::map<std::string, std::string> args;
    std::string fault;
    upnp::QueueActions q([&](const std::string &a, const upnp::SoapArguments &p) {
        action = a;
        args = {p.begin(), p.end()};
        upnp::SoapResult r;
        r.ok = fault.empty();
        r.faultCode = fault;
        return r;
    });
    q.add("http://host/one", "didl");
    assert(action == "AddURIToQueue" && args["EnqueuedURI"] == "http://host/one");
    q.addMultiple({{"http://host/one", "didl1"}, {"http://host/two", "didl2"}}, 42);
    assert(action == "AddMultipleURIsToQueue" && args["UpdateID"] == "42" && args["NumberOfURIs"] == "2");
    assert(args["EnqueuedURIs"] == "http://host/one http://host/two");
    q.remove(43, 2, 3);
    assert(args["UpdateID"] == "43" && args["StartingIndex"] == "2" && args["NumberOfTracks"] == "3");
    fault = "804";
    assert(q.clear().ok);
    fault = "701";
    assert(!q.clear().ok);
    fault = "";
    q.save("Before YeneY 2026-10-18 12:34");
    assert(args["Title"] == "Before YeneY 2026-10-18 12:34");
    q.selectQueue("RINCON_TEST");
    assert(args["CurrentURI"] == "x-rincon-queue:RINCON_TEST#0");
    q.seekTrack(2);
    assert(args["Unit"] == "TRACK_NR" && args["Target"] == "2");
    q.seekTime("0:01:30");
    assert(args["Unit"] == "REL_TIME");
    q.pause();
    assert(action == "Pause" && args.size() == 1);
    q.play();
    assert(action == "Play");
    q.stop();
    assert(action == "Stop");
    q.position();
    assert(action == "GetPositionInfo");
    q.media();
    assert(action == "GetMediaInfo");
    q.crossfade();
    assert(action == "GetCrossfadeMode");
    q.crossfade(false);
    assert(args["CrossfadeMode"] == "0");
    std::cout << "PASS queue actions: batch/removal UpdateID, empty-queue 804, backup, queue URI, track/time "
                 "seek and native transport/crossfade\n";
}
