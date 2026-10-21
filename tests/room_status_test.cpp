// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "room_status.h"
#include <cassert>
#include <iostream>
int main() {
    bridge::RoomStatus status;
    bridge::ModeStatus value;
    value.requested=value.active="queue";value.state="PLAYING";
    value.owned=true;value.occurrence=7;
    value.currentUri="http://local/item?x=1&y=2";value.ownedUris={value.currentUri};
    status.publish(value);
    auto json=bridge::RoomStatus::json(status.status());
    assert(json.find("\"state\":\"PLAYING\"")!=std::string::npos);
    assert(json.find("\"owned\":true")!=std::string::npos);
    assert(json.find("\"owned_item_uris\":[\""+value.currentUri+"\"]")!=std::string::npos);
    assert(json.find("generation")==std::string::npos&&json.find("epoch")==std::string::npos);
    value.state="STOPPED";value.owned=false;value.currentUri.clear();value.ownedUris.clear();value.lastFault="SOAP rejected";
    status.publish(value);
    json=bridge::RoomStatus::json(status.status());
    assert(json.find("SOAP rejected")!=std::string::npos&&json.find("\"owned_item_uris\":[]")!=std::string::npos);
    std::cout<<"PASS relay status: synchronous complete snapshots, state/owned URIs/current/fault, no epochs or generations\n";
}
