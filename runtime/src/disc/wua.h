// Wii U archives (.wua): Cemu's ZArchive format, the decrypted title folders (code/, content/,
// meta/) in zstd-compressed 64 KiB blocks. Unlike a disc image it needs no keys. Format as in
// https://github.com/Exzap/ZArchive (MIT-0). Used by the Android app to extract the user's own
// archive on the device.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace disc {

class Archive {
public:
    // true if `fd` holds a ZArchive (checks the footer only)
    static bool is_archive(int fd);

    // `fd` stays owned by the caller and must stay open; false and `err` on failure. Picks the
    // base game's title folder ("00050000xxxxxxxx_vN"), not updates or DLC.
    bool open(int fd, std::string& err);
    std::string title_id() const { return titleId_; }

    struct File {
        std::string path;  // e.g. "code/cking.rpx", relative to the title folder
        uint64_t offset, size;
    };
    const std::vector<File>& files() const { return files_; }

    // writes the title's files under `outDir` (code/, content/, meta/), like disc::Image::extract
    bool extract(const std::string& outDir, const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress,
                 std::string& err);

private:
    struct Node {
        uint32_t nameAndType;  // MSB: file; lower 31 bits: offset in the name table
        uint32_t a, b, c;      // file: offset low, size low, (size high << 16 | offset high); dir: first node, count
    };
    std::string name(const Node& n) const;
    bool list(uint32_t dir, const std::string& prefix, int depth, std::string& err);
    // the uncompressed block `index` into `out` (64 KiB); `scratch` holds the compressed bytes
    bool read_block(uint64_t index, uint8_t* out, std::vector<uint8_t>& scratch, void* dctx) const;

    int fd_ = -1;
    uint64_t dataOffset_ = 0, dataSize_ = 0;
    std::vector<std::pair<uint64_t, uint32_t>> blocks_;  // (offset in the data section, compressed size)
    std::vector<uint8_t> names_;
    std::vector<Node> nodes_;
    std::string titleId_;
    std::vector<File> files_;
};

}  // namespace disc
