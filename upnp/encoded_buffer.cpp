#include "encoded_buffer.h"
#include <cassert>
#include <cstring>
#include <mutex>
#include <vector>
namespace upnp {
struct EncodedBuffer::Impl {
    std::mutex mutex;
    std::vector<Packet*> slots;
    size_t reader = 0, writer = 0, queued = 0;
    bool warned = false;
    std::function<void(int)> overwritten;
    Impl(int n, std::function<void(int)> callback) : slots(n, nullptr), overwritten(std::move(callback)) {}
    ~Impl() { for (auto p : slots) release(p); }
    static void release(Packet* p) { if (p) { delete[] p->data; delete p; } }
    void advanceReader() { while (!slots[reader]) reader = (reader + 1) % slots.size(); }
};
EncodedBuffer::EncodedBuffer(int n, std::function<void(int)> overwritten) {
    assert(n > 0);
    impl.reset(new Impl(n, std::move(overwritten)));
}
EncodedBuffer::~EncodedBuffer() = default;
void EncodedBuffer::clear() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    for (auto& p : impl->slots) { Impl::release(p); p = nullptr; }
    impl->queued = 0;
    impl->reader = impl->writer;
}
int EncodedBuffer::bytesAvailable() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->queued) return 0;
    impl->advanceReader();
    return impl->slots[impl->reader]->size;
}
int EncodedBuffer::write(const char* p, int n) {
    if (n <= 0) return n;
    auto data = std::unique_ptr<char[]>(new char[n]);
    std::memcpy(data.get(), p, n);
    auto packet = std::unique_ptr<Packet>(new Packet{n, data.get(), nullptr});
    data.release();
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto& slot = impl->slots[impl->writer];
    if (slot) {
        Impl::release(slot);
        if (!impl->warned) {
            impl->warned = true;
            if (impl->overwritten) impl->overwritten(impl->slots.size());
        }
    } else ++impl->queued;
    slot = packet.release();
    impl->writer = (impl->writer + 1) % impl->slots.size();
    return n;
}
EncodedBuffer::Packet* EncodedBuffer::read() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->queued) return nullptr;
    impl->advanceReader();
    auto p = impl->slots[impl->reader];
    impl->slots[impl->reader] = nullptr;
    impl->reader = (impl->reader + 1) % impl->slots.size();
    --impl->queued;
    return p;
}
void EncodedBuffer::freePacket(Packet* p) { Impl::release(p); }
int32_t littleEndianSample(const char* p, unsigned bits) {
    const unsigned width = bits == 16 ? 16 : bits == 24 ? 24 : 32;
    uint32_t value = 0;
    for (unsigned i = 0; i < width / 8; ++i)
        value |= uint32_t(static_cast<unsigned char>(p[i])) << (8 * i);
    const int64_t signedValue = value & (uint32_t(1) << (width - 1))
        ? int64_t(value) - (int64_t(1) << width) : value;
    return static_cast<int32_t>(signedValue);
}
}
