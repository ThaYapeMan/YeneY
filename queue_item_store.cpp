// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_item_store.h"
#include "pcm_pack.h"
#include "wav_writer.h"
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

namespace queue_audio {
namespace {
constexpr size_t chunkFrames = 4096, frameBytes = 6;
struct MemoryFile {
    std::vector<uint8_t> bytes;
    size_t cursor = 0;
    bool failed = false;
};
FLAC__StreamEncoderWriteStatus encoded(const FLAC__StreamEncoder *, const FLAC__byte *p, size_t n, unsigned,
                                       unsigned, void *opaque) {
    auto &m = *static_cast<MemoryFile *>(opaque);
    try {
        if (m.cursor + n > m.bytes.size())
            m.bytes.resize(m.cursor + n);
        std::memcpy(m.bytes.data() + m.cursor, p, n);
        m.cursor += n;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    } catch (...) {
        m.failed = true;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
}
FLAC__StreamEncoderSeekStatus seekEncoded(const FLAC__StreamEncoder *, FLAC__uint64 pos, void *opaque) {
    auto &m = *static_cast<MemoryFile *>(opaque);
    if (pos > m.bytes.size())
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    m.cursor = pos;
    return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}
FLAC__StreamEncoderTellStatus tellEncoded(const FLAC__StreamEncoder *, FLAC__uint64 *pos, void *opaque) {
    *pos = static_cast<MemoryFile *>(opaque)->cursor;
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}
std::vector<uint8_t> compress(const std::vector<char> &pcm, uint32_t rate) {
    MemoryFile memory;
    auto encoder = std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)>(
        FLAC__stream_encoder_new(), FLAC__stream_encoder_delete);
    if (!encoder)
        throw std::bad_alloc();
    auto *e = encoder.get();
    bool ok =
        FLAC__stream_encoder_set_verify(e, true) && FLAC__stream_encoder_set_channels(e, 2) &&
        FLAC__stream_encoder_set_bits_per_sample(e, 24) && FLAC__stream_encoder_set_sample_rate(e, rate) &&
        FLAC__stream_encoder_set_compression_level(e, 5) && FLAC__stream_encoder_set_blocksize(e, 4096) &&
        FLAC__stream_encoder_set_do_mid_side_stereo(e, false) &&
        FLAC__stream_encoder_set_total_samples_estimate(e, pcm.size() / frameBytes);
    if (!ok || FLAC__stream_encoder_init_stream(e, encoded, seekEncoded, tellEncoded, nullptr, &memory) !=
                   FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        throw std::runtime_error("history FLAC init failed");
    std::vector<FLAC__int32> samples(pcm.size() / 3);
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto *p = reinterpret_cast<const unsigned char *>(pcm.data() + i * 3);
        uint32_t n = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
        samples[i] = int32_t(n & 0x800000 ? int64_t(n) - 0x1000000 : n);
    }
    ok = FLAC__stream_encoder_process_interleaved(e, samples.data(), pcm.size() / frameBytes);
    ok = FLAC__stream_encoder_finish(e) && ok;
    if (!ok || memory.failed)
        throw std::runtime_error("history FLAC encode failed");
    return std::move(memory.bytes);
}
struct Decoded {
    std::vector<char> bytes;
    size_t expected;
    uint32_t rate;
    bool failed = false;
};
FLAC__StreamDecoderWriteStatus decoded(const FLAC__StreamDecoder *, const FLAC__Frame *f,
                                       const FLAC__int32 *const channels[], void *opaque) {
    auto &d = *static_cast<Decoded *>(opaque);
    if (f->header.channels != 2 || f->header.bits_per_sample != 24 || f->header.sample_rate != d.rate ||
        f->header.blocksize * frameBytes > d.expected - d.bytes.size()) {
        d.failed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    try {
        for (unsigned i = 0; i < f->header.blocksize; ++i)
            for (unsigned c = 0; c < 2; ++c)
                for (unsigned b = 0; b < 3; ++b)
                    d.bytes.push_back(char(uint32_t(channels[c][i]) >> (8 * b)));
    } catch (...) {
        d.failed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
void decodeError(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *opaque) {
    static_cast<Decoded *>(opaque)->failed = true;
}
std::vector<char> decompress(FILE *file, size_t frames, uint32_t rate) {
    Decoded data{{}, frames * frameBytes, rate};
    data.bytes.reserve(data.expected);
    auto decoder = std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>(
        FLAC__stream_decoder_new(), FLAC__stream_decoder_delete);
    if (!decoder) {
        fclose(file);
        throw std::bad_alloc();
    }
    FLAC__stream_decoder_set_md5_checking(decoder.get(), true);
    if (FLAC__stream_decoder_init_FILE(decoder.get(), file, decoded, nullptr, decodeError, &data) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK)
        throw std::runtime_error("history FLAC open failed");
    bool ok = FLAC__stream_decoder_process_until_end_of_stream(decoder.get());
    ok = FLAC__stream_decoder_finish(decoder.get()) && ok;
    if (!ok || data.failed || data.bytes.size() != data.expected)
        throw std::runtime_error("history FLAC corrupt");
    return std::move(data.bytes);
}
} // namespace
LengthFault::LengthFault(uint64_t e, uint64_t a)
    : std::runtime_error("item length fault expected=" + std::to_string(e) + " actual=" + std::to_string(a)),
      expected(e), actual(a) {}
Config Config::environment() {
    Config c;
    if (const char *p = std::getenv("YENEY_QUEUE_DISK_MIB")) {
        std::string s(p);
        size_t used = 0;
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("invalid YENEY_QUEUE_DISK_MIB");
        auto mib = std::stoull(s, &used);
        if (!mib || used != s.size() || mib > UINT64_MAX / (1024 * 1024))
            throw std::invalid_argument("invalid YENEY_QUEUE_DISK_MIB");
        c.diskBytes = mib * 1024 * 1024;
    }
    return c;
}
struct RamChunk {
    uint64_t first;
    std::vector<char> bytes;
};
struct HistoryChunk {
    uint64_t first;
    size_t frames;
    std::filesystem::path path;
    uint64_t bytes;
};
struct ItemState {
    const Descriptor descriptor;
    const uint64_t serial;
    uint64_t produced = 0;
    bool finished = false, protectedItem = false;
    std::exception_ptr fault;
    std::deque<RamChunk> ram;
    std::vector<HistoryChunk> history;
    ItemState(Descriptor d, uint64_t id) : descriptor(std::move(d)), serial(id) {}
};
struct StoreState {
    mutable std::mutex mutex;
    std::mutex producer; // one storage writer; never held by readers/cancellation
    std::condition_variable changed;
    Config config;
    std::filesystem::path directory;
    std::map<std::string, std::shared_ptr<ItemState>> items;
    uint64_t serial = 0, disk = 0;
    size_t ram = 0;
    bool stopped = false;
    int lockFd = -1;
    ~StoreState() {
        if (lockFd >= 0)
            close(lockFd);
    }
    void check(const ItemState &item) const {
        if (item.fault)
            std::rethrow_exception(item.fault);
        if (stopped)
            throw std::runtime_error("item store closed");
    }
    void fail(ItemState &item, std::exception_ptr fault) {
        item.fault = std::move(fault);
        for (const auto &chunk : item.ram)
            ram -= chunk.bytes.capacity();
        item.ram.clear();
        changed.notify_all();
    }
    void space(uint64_t amount, const ItemState &writing) {
        while (amount > config.diskBytes - disk) {
            std::shared_ptr<ItemState> oldest;
            for (const auto &pair : items) {
                const auto &i = pair.second;
                if (i.get() != &writing && (i->finished || i->fault) && !i->protectedItem &&
                    !i->history.empty() && (!oldest || i->serial < oldest->serial))
                    oldest = i;
            }
            if (!oldest)
                throw std::runtime_error("queue disk quota exhausted: current/next retained");
            for (const auto &h : oldest->history) {
                if (!std::filesystem::remove(h.path))
                    throw std::runtime_error("history eviction failed");
                disk -= h.bytes;
            }
            oldest->history.clear();
            oldest->fault =
                std::make_exception_ptr(std::runtime_error("item history evicted; LMS re-render required"));
            changed.notify_all();
        }
    }
    void spill(ItemState &item, std::unique_lock<std::mutex> &lock) {
        // Snapshot RAM under the metadata lock, then encode/write without it.
        // The single writer owns producer; cancellation may still wake readers.
        auto snapshot = item.ram.front();
        lock.unlock();
        std::vector<uint8_t> flac;
        try {
            flac = compress(snapshot.bytes, item.descriptor.rate);
        } catch (...) {
            lock.lock();
            throw;
        }
        lock.lock();
        check(item);
        auto &r = item.ram.front();
        space(flac.size(), item);
        const auto path = directory / (std::to_string(item.serial) + "-" + std::to_string(r.first) + ".flac");
        // FLAC is fully encoded in bounded temporary RAM before quota admission.
        // No WAV or raw PCM file is ever written.
        if (item.history.size() == item.history.capacity())
            item.history.reserve(item.history.capacity() ? item.history.capacity() * 2 : 16);
        HistoryChunk record{r.first, r.bytes.size() / frameBytes, path, flac.size()};
        lock.unlock();
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(flac.data()), flac.size());
        file.close();
        if (!file) {
            lock.lock();
            std::filesystem::remove(path);
            throw std::runtime_error("history write failed");
        }
        lock.lock();
        try {
            check(item);
        } catch (...) {
            std::filesystem::remove(path);
            throw;
        }
        item.history.push_back(std::move(record));
        disk += flac.size();
        ram -= r.bytes.capacity();
        item.ram.pop_front();
    }
    void trimRam(size_t reserve, std::unique_lock<std::mutex> &lock) {
        while (reserve > config.ramBytes - ram) {
            std::shared_ptr<ItemState> oldest;
            for (const auto &pair : items)
                if (!pair.second->ram.empty() && (!oldest || pair.second->serial < oldest->serial))
                    oldest = pair.second;
            spill(*oldest, lock);
        }
    }
};
Store::Store(const std::string &room, Config c) : state(std::make_shared<StoreState>()) {
    if (room.empty() || room == "." || room == ".." || room.find('/') != std::string::npos ||
        room.find('\0') != std::string::npos || c.ramBytes < frameBytes || !c.diskBytes)
        throw std::invalid_argument("invalid queue store configuration");
    state->config = std::move(c);
    state->directory = state->config.root / room;
    if (std::filesystem::is_symlink(state->config.root) || std::filesystem::is_symlink(state->directory))
        throw std::runtime_error("queue storage directory is a symlink");
    std::filesystem::create_directories(state->directory);
    std::filesystem::permissions(state->directory, std::filesystem::perms::owner_all);
    state->lockFd =
        open((state->directory / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (state->lockFd < 0 || flock(state->lockFd, LOCK_EX | LOCK_NB))
        throw std::runtime_error("queue room store already open or unavailable");
    for (const auto &entry : std::filesystem::directory_iterator(state->directory))
        if (entry.path().filename() != ".lock")
            std::filesystem::remove_all(entry.path());
}
Store::~Store() {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->stopped = true;
    state->changed.notify_all();
    if (state->lockFd >= 0) {
        close(state->lockFd);
        state->lockFd = -1;
    }
}
std::shared_ptr<Item> Store::create(Descriptor descriptor) {
    if (descriptor.occurrence.empty() || descriptor.occurrence.find('\0') != std::string::npos ||
        !descriptor.rate || descriptor.rate > 655350 || descriptor.channels != 2 || descriptor.bits != 24)
        throw std::invalid_argument("invalid item descriptor");
    if (descriptor.promisedFrames)
        wavLength(*descriptor.promisedFrames);
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto key = std::to_string(descriptor.generation) + ":" + descriptor.occurrence;
    if (state->items.count(key))
        throw std::invalid_argument("duplicate occurrence/generation");
    auto item = std::make_shared<ItemState>(std::move(descriptor), ++state->serial);
    state->items.emplace(key, item);
    return std::shared_ptr<Item>(new Item(state, item));
}
std::shared_ptr<Item> Store::publish(const std::shared_ptr<Item> &pending, uint64_t promised) {
    wavLength(promised);
    std::lock_guard<std::mutex> writer(state->producer);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!pending || pending->store != state)
        throw std::invalid_argument("foreign pending item");
    auto &old = *pending->state;
    state->check(old);
    if (old.descriptor.promisedFrames || old.finished || old.produced > promised)
        throw std::invalid_argument("publication conflicts with produced frames");
    auto descriptor = old.descriptor;
    descriptor.promisedFrames = promised;
    ++descriptor.generation;
    const auto key = std::to_string(descriptor.generation) + ":" + descriptor.occurrence;
    if (state->items.count(key))
        throw std::invalid_argument("duplicate publication generation");
    auto item = std::make_shared<ItemState>(descriptor, old.serial);
    item->produced = old.produced;
    item->ram = std::move(old.ram);
    item->history = std::move(old.history);
    item->protectedItem = old.protectedItem;
    old.fault = std::make_exception_ptr(std::runtime_error("pending item superseded by publication"));
    state->items.emplace(key, item);
    state->changed.notify_all();
    return std::shared_ptr<Item>(new Item(state, item));
}
void Store::protect(const std::shared_ptr<Item> &current, const std::shared_ptr<Item> &next) {
    std::lock_guard<std::mutex> lock(state->mutex);
    for (auto item : {current, next})
        if (item && item->store != state)
            throw std::invalid_argument("foreign protected item");
    for (auto &pair : state->items)
        pair.second->protectedItem = false;
    for (auto item : {current, next})
        if (item)
            item->state->protectedItem = true;
}
uint64_t Store::diskBytes() const {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->disk;
}
size_t Store::ramBytes() const {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->ram;
}
Item::Item(std::shared_ptr<StoreState> s, std::shared_ptr<ItemState> i)
    : store(std::move(s)), state(std::move(i)) {}
const Descriptor &Item::descriptor() const { return state->descriptor; }
std::string Item::room() const { return store->directory.filename().string(); }
std::optional<uint64_t> Item::contentLength() const {
    std::lock_guard<std::mutex> lock(store->mutex);
    store->check(*state);
    if (state->descriptor.promisedFrames)
        return wavLength(*state->descriptor.promisedFrames);
    return state->finished ? std::optional<uint64_t>(wavLength(state->produced)) : std::nullopt;
}
uint64_t Item::producedFrames() const {
    std::lock_guard<std::mutex> lock(store->mutex);
    return state->produced;
}
void Item::append(const yeney::Frame *frames, size_t count, bool processed) {
    std::lock_guard<std::mutex> writer(store->producer);
    std::unique_lock<std::mutex> lock(store->mutex);
    store->check(*state);
    if (state->finished || (!frames && count))
        throw std::invalid_argument("invalid item append");
    try {
        if (count > UINT64_MAX - state->produced)
            throw std::overflow_error("frame count overflow");
        auto actual = state->produced + count;
        if (state->descriptor.promisedFrames && actual > *state->descriptor.promisedFrames)
            throw LengthFault(*state->descriptor.promisedFrames, actual);
        wavLength(actual);
        PcmDither unused;
        while (count) {
            const size_t allocation = std::min(chunkFrames, store->config.ramBytes / frameBytes) * frameBytes;
            if (state->ram.empty() || state->ram.back().bytes.size() == allocation) {
                store->trimRam(allocation, lock);
                RamChunk chunk{state->produced, {}};
                chunk.bytes.reserve(allocation);
                state->ram.push_back(std::move(chunk));
                store->ram += state->ram.back().bytes.capacity();
            }
            auto &bytes = state->ram.back().bytes;
            const size_t n = std::min(count, (bytes.capacity() - bytes.size()) / frameBytes);
            const auto base = bytes.size();
            bytes.resize(base + n * frameBytes);
            for (size_t i = 0; i < n; ++i) {
                packSonosSample(bytes.data() + base + i * 6, frames[i].left, 3, processed, unused);
                packSonosSample(bytes.data() + base + i * 6 + 3, frames[i].right, 3, processed, unused);
            }
            state->produced += n;
            count -= n;
            frames += n;
        }
    } catch (...) {
        store->fail(*state, std::current_exception());
        throw;
    }
    store->changed.notify_all();
}
void Item::finish() {
    std::lock_guard<std::mutex> writer(store->producer);
    std::unique_lock<std::mutex> lock(store->mutex);
    store->check(*state);
    if (state->finished)
        return;
    try {
        if (state->descriptor.promisedFrames && state->produced != *state->descriptor.promisedFrames)
            throw LengthFault(*state->descriptor.promisedFrames, state->produced);
        while (!state->ram.empty())
            store->spill(*state, lock);
        state->finished = true;
    } catch (...) {
        store->fail(*state, std::current_exception());
        throw;
    }
    store->changed.notify_all();
}
void Item::cancel(const std::string &reason) {
    std::lock_guard<std::mutex> lock(store->mutex);
    if (!state->fault)
        store->fail(*state, std::make_exception_ptr(std::runtime_error(reason)));
}
Reader Item::reader(uint64_t pos) const { return Reader(store, state, pos); }
Reader::Reader(std::shared_ptr<StoreState> s, std::shared_ptr<ItemState> i, uint64_t pos)
    : store(std::move(s)), state(std::move(i)), offset(pos) {}
size_t Reader::read(char *output, size_t capacity, std::chrono::milliseconds timeout) {
    if (!capacity)
        return 0;
    if (!output)
        throw std::invalid_argument("null read buffer");
    std::unique_lock<std::mutex> lock(store->mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        store->check(*state);
        auto frames = state->descriptor.promisedFrames;
        if (!frames && state->finished)
            frames = state->produced;
        if (frames) {
            const auto length = wavLength(*frames);
            if (offset > length)
                throw std::out_of_range("range beyond item");
            if (offset == length)
                return 0;
            if (offset < 44) {
                const auto h = wavHeader(state->descriptor.rate, *frames);
                const auto n = std::min<uint64_t>(capacity, 44 - offset);
                std::memcpy(output, h.data() + offset, n);
                offset += n;
                return n;
            }
            const uint64_t pcmOffset = offset - 44;
            if (pcmOffset < state->produced * frameBytes) {
                auto history =
                    std::upper_bound(state->history.begin(), state->history.end(), pcmOffset / frameBytes,
                                     [](uint64_t frame, const HistoryChunk &h) { return frame < h.first; });
                if (history != state->history.begin()) {
                    const auto h = *--history;
                    if (pcmOffset < (h.first + h.frames) * frameBytes) {
                        if (decodedFirst != h.first) {
                            try {
                                // Open while indexed; an open inode survives concurrent
                                // eviction.
                                FILE *file = fopen(h.path.c_str(), "rb");
                                if (!file)
                                    throw std::runtime_error("history FLAC open failed");
                                const auto first = h.first;
                                const auto frames = h.frames;
                                lock.unlock();
                                try {
                                    decodedChunk = decompress(file, frames, state->descriptor.rate);
                                } catch (...) {
                                    lock.lock();
                                    throw;
                                }
                                lock.lock();
                                store->check(*state);
                                decodedFirst = first;
                            } catch (...) {
                                store->fail(*state, std::current_exception());
                                throw;
                            }
                            decodedFirst = h.first;
                        }
                        const auto start = pcmOffset - h.first * frameBytes;
                        const auto n = std::min<uint64_t>(capacity, decodedChunk.size() - start);
                        std::memcpy(output, decodedChunk.data() + start, n);
                        offset += n;
                        return n;
                    }
                }
                for (const auto &r : state->ram) {
                    if (pcmOffset >= r.first * frameBytes &&
                        pcmOffset < r.first * frameBytes + r.bytes.size()) {
                        const auto start = pcmOffset - r.first * frameBytes;
                        const auto n = std::min<uint64_t>(capacity, r.bytes.size() - start);
                        std::memcpy(output, r.bytes.data() + start, n);
                        offset += n;
                        return n;
                    }
                }
                throw std::runtime_error("missing produced history");
            }
        }
        if (store->changed.wait_until(lock, deadline) == std::cv_status::timeout) {
            store->check(*state);
            throw ReadTimeout();
        }
    }
}
} // namespace queue_audio
