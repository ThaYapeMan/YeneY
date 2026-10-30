// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "third_party/yeney-core/core/sink.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace queue_audio {
enum class PromiseKind { Exact, UpperBound };
constexpr unsigned remoteMarginSeconds = 5;
struct Descriptor {
    std::string occurrence;
    uint32_t rate;
    std::optional<uint64_t> promisedFrames; // Exact or UpperBound; nullopt: sealed publication
    uint64_t generation;
    unsigned channels = 2, bits = 24;
    PromiseKind kind = PromiseKind::Exact;
};
struct PublicationSuperseded : std::runtime_error {
    PublicationSuperseded() : std::runtime_error("pending item superseded by publication") {}
};
struct ItemCancelled : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct LengthFault : std::runtime_error {
    const uint64_t expected, actual;
    LengthFault(uint64_t expected, uint64_t actual);
};
struct ReadTimeout : std::runtime_error {
    ReadTimeout() : std::runtime_error("item read timeout") {}
};
struct Config {
    std::filesystem::path root = "/var/lib/yeney/queue";
    size_t ramBytes = 8 * 1024 * 1024;
    uint64_t diskBytes = 256ull * 1024 * 1024;
    uint64_t estimatedDiskBytes = 8ull * 1024 * 1024 * 1024;
    static Config environment(); // YENEY_QUEUE_DISK_MIB; invalid values fail
};
struct StoreState;
struct ItemState;
class Reader;
class Item {
    friend class Store;
    friend class Reader;
    std::shared_ptr<StoreState> store;
    std::shared_ptr<ItemState> state;
    Item(std::shared_ptr<StoreState>, std::shared_ptr<ItemState>);

public:
    const Descriptor &descriptor() const;
    std::string room() const;
    std::optional<uint64_t> contentLength() const;
    uint64_t producedFrames() const;
    bool finished() const;
    void append(const yeney::Frame *, size_t count, bool processed);
    void finish(); // Exact requires count; UpperBound permits short EOF; pending seals
    void cancel(const std::string &reason = "cancelled");
    Reader reader(uint64_t byteOffset = 0) const;
};
class Reader {
    friend class Item;
    std::shared_ptr<StoreState> store;
    std::shared_ptr<ItemState> state;
    uint64_t offset;
    uint64_t decodedFirst = UINT64_MAX;
    std::vector<char> decodedChunk; // bounded 4096-frame per-reader decode scratch
    Reader(std::shared_ptr<StoreState>, std::shared_ptr<ItemState>, uint64_t);

public:
    uint64_t position() const { return offset; }
    // Partial reads are allowed. Future ranges wait; with nonzero capacity,
    // 0 means entity EOF only.
    // Cancellation, eviction, length faults and timeout throw, never fake EOF.
    size_t read(char *, size_t capacity, std::chrono::milliseconds timeout = std::chrono::seconds(15));
};
class Store {
    std::shared_ptr<StoreState> state;

public:
    explicit Store(const std::string &room, Config config = Config::environment());
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    std::shared_ptr<Item> create(Descriptor);
    std::shared_ptr<Item> publish(const std::shared_ptr<Item> &, uint64_t promisedFrames, PromiseKind kind = PromiseKind::Exact);
    void protect(const std::shared_ptr<Item> &current, const std::shared_ptr<Item> &next);
    void protect(const std::vector<std::shared_ptr<Item>> &items);
    std::optional<yeney::EncodedSpool> reserveEncoded(bool enabled);
    uint64_t diskBytes() const;
    size_t ramBytes() const;
};
} // namespace queue_audio
