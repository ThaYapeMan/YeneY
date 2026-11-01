// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "room_status.h"
#include <cassert>
#include <iostream>
int main() {
    assert(queueUriLog("http://bridge/yeney/q/secret/1/1/lazy-7.wav") == "http://bridge/yeney/q/.../1/1/lazy-7.wav");
    bridge::RoomStatus status;
    bridge::ModeStatus value;
    value.requested=value.active="queue";value.state="PLAYING";
    value.path="relay";value.title=std::string("é & <live> ")+char(34)+"mix"+char(34)+char(10);
    value.owned=true;value.occurrence=7;value.renderedRate=44100;
    value.currentUri="http://local/yeney/q/secret/1/1/lazy-7.wav";value.ownedUris={value.currentUri};
    status.publish(value);
    auto json=bridge::RoomStatus::json(status.status());
    assert(json.find("\"state\":\"PLAYING\"")!=std::string::npos);
    assert(json.find("\"owned\":true")!=std::string::npos);
    assert(json.find("\"owned_item_uris\":[\""+queueUriLog(value.currentUri)+"\"]")!=std::string::npos);
    assert(json.find("secret") == std::string::npos);
    assert(json.find("\"rendered_rate\":44100")!=std::string::npos);
    assert(json.find(bridge::RoomStatus::quote(value.title))!=std::string::npos);
    assert(status.status().title==value.title);
    assert(json.find("generation")==std::string::npos&&json.find("epoch")==std::string::npos);
    value.state="STOPPED";value.owned=false;value.currentUri.clear();value.ownedUris.clear();value.lastFault="SOAP rejected";
    status.publish(value);
    json=bridge::RoomStatus::json(status.status());
    assert(json.find("SOAP rejected")!=std::string::npos&&json.find("\"owned_item_uris\":[]")!=std::string::npos);
    std::cout<<"PASS relay status: synchronous complete snapshots, state/owned URIs/current/fault, no epochs or generations\n";
}
