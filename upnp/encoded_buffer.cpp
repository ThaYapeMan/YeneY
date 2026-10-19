#include "encoded_buffer.h"
#include "../stream_metrics.h"
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>
namespace upnp {
struct EncodedBuffer::Impl {
    std::mutex mutex;
    std::vector<Packet*> slots;
    size_t reader = 0, writer = 0, queued = 0;
    std::condition_variable changed;
    std::chrono::milliseconds stall;
    std::chrono::steady_clock::time_point progress = std::chrono::steady_clock::now();
    std::function<void(int)> overwritten;
    Impl(int n, std::function<void(int)> callback, std::chrono::milliseconds limit) : slots(n, nullptr), stall(limit), overwritten(std::move(callback)) {}
    ~Impl() { for (auto p : slots) release(p); }
    static void release(Packet* p) { if (p) { delete[] p->data; delete p; } }
    void advanceReader() { while (!slots[reader]) reader = (reader + 1) % slots.size(); }
};
EncodedBuffer::EncodedBuffer(int n, std::function<void(int)> overwritten, std::chrono::milliseconds stall) {
    assert(n > 0);
    assert(stall.count() > 0);
    impl.reset(new Impl(n, std::move(overwritten), stall));
}
EncodedBuffer::~EncodedBuffer() = default;
void EncodedBuffer::clear() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    for (auto& p : impl->slots) { Impl::release(p); p = nullptr; }
    impl->queued = 0;
    impl->reader = impl->writer;
    impl->progress = std::chrono::steady_clock::now();
    impl->changed.notify_all();
}
int EncodedBuffer::bytesAvailable() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->queued) return 0;
    impl->advanceReader();
    return impl->slots[impl->reader]->size;
}
int EncodedBuffer::write(const char* p, int n, const std::function<bool()> &interrupted) {
    if (n <= 0) return n;
    auto data = std::unique_ptr<char[]>(new char[n]);
    std::memcpy(data.get(), p, n);
    auto packet = std::unique_ptr<Packet>(new Packet{n, data.get(), nullptr});
    data.release();
    std::unique_lock<std::mutex> lock(impl->mutex);
    auto began = std::chrono::steady_clock::now();
    while (impl->queued == impl->slots.size()) {
        if (interrupted && interrupted()) {
            Impl::release(packet.release()); return 0;
        }
        auto deadline = std::max(began, impl->progress) + impl->stall;
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            impl->advanceReader();
            Impl::release(impl->slots[impl->reader]);
            impl->slots[impl->reader] = nullptr;
            impl->reader = (impl->reader+1)%impl->slots.size();
            --impl->queued;
            ++bridge::streamMetrics().dropped;
            if (impl->overwritten) impl->overwritten(impl->slots.size());
            break; // one oldest packet per full-buffer stall interval
        }
        impl->changed.wait_until(lock, std::min(deadline, now+std::chrono::milliseconds(20)));
    }
    if (interrupted && interrupted()) { Impl::release(packet.release()); return 0; }
    auto& slot = impl->slots[impl->writer];
    assert(!slot);
    ++impl->queued;
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
    impl->progress = std::chrono::steady_clock::now();
    impl->changed.notify_all();
    return p;
}
void EncodedBuffer::readerProgress() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->progress = std::chrono::steady_clock::now();
    impl->changed.notify_all();
}
void EncodedBuffer::freePacket(Packet* p) { Impl::release(p); if (p) readerProgress(); }
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
