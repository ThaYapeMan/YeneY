#include "upnp/encoded_buffer.h"
#include <cassert>
#include <cstring>
#include <iostream>
using upnp::EncodedBuffer;
static void packet(EncodedBuffer& buffer, const char* value) {
    assert(buffer.bytesAvailable() == int(std::strlen(value)));
    auto p = buffer.read();
    assert(p && p->size == int(std::strlen(value)));
    assert(std::memcmp(p->data, value, p->size) == 0);
    buffer.freePacket(p);
}
int main() {
    EncodedBuffer buffer(3);
    assert(buffer.bytesAvailable() == 0 && !buffer.read());
    assert(buffer.write("", 0) == 0 && !buffer.read());
    assert(buffer.write("abc", 3) == 3);
    assert(buffer.write("de", 2) == 2);
    assert(buffer.write("f", 1) == 1);
    packet(buffer, "abc");
    assert(buffer.write("ghij", 4) == 4); // wrap into the first slot
    packet(buffer, "de");
    packet(buffer, "f");
    packet(buffer, "ghij");
    assert(!buffer.read());
    for (const auto value : {"one", "two", "three", "four"})
        assert(buffer.write(value, std::strlen(value)) == int(std::strlen(value)));
    packet(buffer, "four"); // overwritten read slot is returned first
    packet(buffer, "two");
    auto held = buffer.read();
    assert(held && held->size == 5);
    buffer.clear();
    assert(!buffer.read() && buffer.bytesAvailable() == 0);
    assert(buffer.write("new", 3) == 3);
    assert(std::memcmp(held->data, "three", 5) == 0);
    buffer.freePacket(held);
    packet(buffer, "new");
    buffer.freePacket(nullptr);
    for (unsigned bits : {16u, 24u, 32u}) {
        char data[5] = {};
        auto p = data + 1; // deliberately unaligned
        assert(upnp::littleEndianSample(p, bits) == 0);
        std::memset(p, 0xff, bits / 8);
        assert(upnp::littleEndianSample(p, bits) == -1);
        std::memset(p, 0, bits / 8);
        p[bits / 8 - 1] = char(0x80);
        assert(upnp::littleEndianSample(p, bits) == -(int64_t(1) << (bits - 1)));
        p[bits / 8 - 1] = 0x7f;
        for (unsigned i = 0; i + 1 < bits / 8; ++i) p[i] = char(0xff);
        assert(upnp::littleEndianSample(p, bits) == (int64_t(1) << (bits - 1)) - 1);
    }
    std::cout << "PASS: packet capacity, full overwrite, FIFO wrap, empty reads, clear with held packet, signed unaligned little-endian samples\n";
}
