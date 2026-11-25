// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace queue_audio {
class PackedFlac {
    struct CloseFile {
        void operator()(FILE *file) const { std::fclose(file); }
    };
    std::unique_ptr<FILE, CloseFile> file;
    std::vector<uint64_t> offsets;
    uint64_t bytes = 0;
    explicit PackedFlac(FILE *file) : file(file) {}

public:
    using ReadPcm = std::function<size_t(char *, size_t)>;
    static std::shared_ptr<const PackedFlac> encode(const std::filesystem::path &, uint64_t samples,
                                                  unsigned rate, unsigned bits, const ReadPcm &, uint64_t limit);
    uint64_t size() const { return bytes; }
    uint64_t offset(size_t index) const { return offsets.at(index); }
    size_t frames() const { return offsets.size() - 1; }
    size_t read(uint64_t offset, char *, size_t) const;
};
}
