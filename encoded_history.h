#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

// Offsets count FLAC entity bytes, never HTTP headers or chunk framing.
class EncodedHistory {
public:
    static constexpr size_t Capacity = 32u * 1024u * 1024u;
    explicit EncodedHistory(size_t capacity = Capacity) : capacity(capacity) {}
    void append(const char* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex);
        const uint64_t next = end + size;
        if (bytes.empty()) bytes.reserve(capacity);
        if (bytes.size() < capacity) bytes.resize(std::min<uint64_t>(capacity, next));
        if (size > capacity) { data += size - capacity; size = capacity; }
        const uint64_t offset = next - size;
        for (size_t at = 0; at < size;) {
            size_t index = (offset + at) % capacity;
            size_t count = std::min(size - at, capacity - index);
            memcpy(bytes.data() + index, data + at, count); at += count;
        }
        end = next; begin = end > capacity ? end - capacity : 0;
    }
    // -1 means evicted; 0 means not produced yet; positive values are copied.
    int read(uint64_t offset, char* output, size_t limit) const {
        std::lock_guard<std::mutex> lock(mutex);
        if (offset < begin) return -1;
        if (offset >= end) return 0;
        size_t size = std::min<uint64_t>(limit, end - offset);
        for (size_t at = 0; at < size;) {
            size_t index = (offset + at) % capacity;
            size_t count = std::min(size - at, capacity - index);
            memcpy(output + at, bytes.data() + index, count); at += count;
        }
        return size;
    }
    std::pair<uint64_t,uint64_t> bounds() const {
        std::lock_guard<std::mutex> lock(mutex); return {begin,end};
    }
private:
    const size_t capacity;
    mutable std::mutex mutex;
    std::vector<char> bytes;
    uint64_t begin = 0, end = 0;
};
inline bool openByteRange(const std::string& value, uint64_t& offset) {
    if (value.compare(0,6,"bytes=") || value.size() < 8 || value.back() != '-') return false;
    offset = 0;
    for (size_t i = 6; i + 1 < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        unsigned digit = value[i] - '0';
        if (offset > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        offset = offset * 10 + digit;
    }
    return true;
}
