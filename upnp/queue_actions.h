// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "soap.h"
#include <functional>
#include <cstdint>
namespace upnp {
// Serialized coordinator-only action boundary. The caller supplies a transport
// bound to its coordinator lease, never a member speaker.
class QueueActions {
public:
    using Call = std::function<SoapResult(const std::string &, const SoapArguments &)>;
    explicit QueueActions(Call call) : call(std::move(call)) {}
    SoapResult add(const std::string &uri, const std::string &metadata, unsigned before = 0);
    SoapResult addMultiple(const std::vector<std::pair<std::string, std::string>> &, uint32_t update,
                           unsigned before = 0);
    SoapResult remove(uint32_t update, unsigned first, unsigned count);
    SoapResult clear();
    SoapResult save(const std::string &title);
    SoapResult selectQueue(const std::string &udn);
    SoapResult seekTrack(unsigned number);
    SoapResult seekTime(const std::string &time);
    SoapResult play();
    SoapResult pause();
    SoapResult stop();
    SoapResult position();
    SoapResult media();
    SoapResult crossfade();
    SoapResult crossfade(bool enabled);

private:
    Call call;
};
} // namespace upnp
