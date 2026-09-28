#pragma once
#include <cstdint>
#include <memory>
#include <functional>
namespace upnp {
// Fixed packet-slot capacity. Writes replace occupied slots without moving the
// read cursor; a replaced read slot is returned first (baseline regression test).
// Read packets stay valid through clear() until freePacket(). One writer/reader.
class EncodedBuffer {
public:
    struct Packet { int size; const char* data; void* native; };
    explicit EncodedBuffer(int capacity, std::function<void(int)> overwritten = {});
    ~EncodedBuffer();
    void clear();
    int bytesAvailable();
    int write(const char*, int);
    Packet* read();
    void freePacket(Packet*);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
int32_t littleEndianSample(const char* data, unsigned bits);
}
