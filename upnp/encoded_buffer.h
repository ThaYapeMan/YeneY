#pragma once
#include <cstdint>
#include <memory>
#include <functional>
#include <chrono>
namespace upnp {
// Fixed packet-slot capacity. A full ring backpressures its writer. Only a
// bounded reader stall permits dropping the oldest queued packet.
// Read packets stay valid through clear() until freePacket(). One writer/reader.
class EncodedBuffer {
public:
    struct Packet { int size; const char* data; void* native; };
    explicit EncodedBuffer(int capacity, std::function<void(int)> dropped = {},
        std::chrono::milliseconds stall = std::chrono::seconds(5));
    ~EncodedBuffer();
    void clear();
    int bytesAvailable();
    int write(const char*, int, const std::function<bool()> &interrupted = {});
    void readerProgress();
    Packet* read();
    void freePacket(Packet*);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
int32_t littleEndianSample(const char* data, unsigned bits);
}
