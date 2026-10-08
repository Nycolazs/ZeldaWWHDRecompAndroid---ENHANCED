// Wii U archive (.wua) reader and extractor (see wua.h).
#include "wua.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

#include <zstd.h>

namespace disc {
namespace {

constexpr uint32_t kBlock = 0x10000, kBlocksPerRecord = 16, kRecordSize = 8 + 2 * kBlocksPerRecord;
constexpr uint32_t kMagic = 0x169F52D6, kVersion1 = 0x61BF3A01;
constexpr size_t kFooterSize = 6 * 16 + 32 + 8 + 4 + 4;

uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool pread_all(int fd, uint64_t offset, void* out, size_t n) {
    auto* o = (uint8_t*)out;
    while (n > 0) {
        ssize_t r = pread(fd, o, n, (off_t)offset);
        if (r <= 0) return false;
        o += r;
        offset += r;
        n -= r;
    }
    return true;
}

bool mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') {
            std::string d = path.substr(0, i);
            if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    return true;
}

bool iequals(const std::string& a, const char* b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// "0005000010143500_v0" -> "0005000010143500"; "" if the name isn't a title folder
std::string title_of(const std::string& dir) {
    if (dir.size() < 16) return "";
    for (int i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)dir[i])) return "";
    if (dir.size() > 16 && dir[16] != '_') return "";
    std::string t = dir.substr(0, 16);
    for (auto& c : t) c = (char)tolower((unsigned char)c);
    return t;
}

struct Footer {
    uint64_t off[6], size[6];  // compressed data, offset records, names, file tree, meta directory, meta data
    uint64_t total;
    uint32_t version, magic;
};

bool read_footer(int fd, Footer& f) {
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size <= kFooterSize) return false;
    uint8_t raw[kFooterSize];
    if (!pread_all(fd, (uint64_t)st.st_size - kFooterSize, raw, kFooterSize)) return false;
    for (int i = 0; i < 6; i++) {
        f.off[i] = be64(raw + i * 16);
        f.size[i] = be64(raw + i * 16 + 8);
    }
    f.total = be64(raw + 6 * 16 + 32);
    f.version = be32(raw + 6 * 16 + 40);
    f.magic = be32(raw + 6 * 16 + 44);
    if (f.magic != kMagic || f.version != kVersion1 || f.total != (uint64_t)st.st_size) return false;
    for (int i = 0; i < 6; i++)
        if (f.off[i] > f.total || f.size[i] > f.total - f.off[i]) return false;
    return true;
}

}  // namespace

bool Archive::is_archive(int fd) {
    Footer f;
    return read_footer(fd, f);
}

std::string Archive::name(const Node& n) const {
    uint32_t o = n.nameAndType & 0x7FFFFFFF;
    if (o == 0x7FFFFFFF || o >= names_.size()) return "";
    uint32_t len = names_[o] & 0x7F;
    if (names_[o] & 0x80) {  // 2-byte length
        if (o + 1 >= names_.size()) return "";
        len |= (uint32_t)names_[o + 1] << 7;
        o += 2;
    } else {
        o += 1;
    }
    if ((uint64_t)o + len > names_.size()) return "";
    return std::string((const char*)names_.data() + o, len);
}

bool Archive::list(uint32_t dir, const std::string& prefix, int depth, std::string& err) {
    if (depth > 64) return err = "corrupt archive (folders nested too deep)", false;
    const Node& d = nodes_[dir];
    if ((uint64_t)d.a + d.b > nodes_.size()) return err = "corrupt archive (file tree)", false;
    for (uint32_t i = d.a; i < d.a + d.b; i++) {
        const Node& n = nodes_[i];
        std::string nm = name(n);
        // no names that would leave the output folder
        if (nm.empty() || nm == "." || nm == ".." || nm.find_first_of(std::string("/\\\0", 3)) != std::string::npos)
            return err = "corrupt archive (bad file name)", false;
        std::string path = prefix.empty() ? nm : prefix + "/" + nm;
        if (n.nameAndType & 0x80000000) {
            uint64_t offset = n.a | (uint64_t)(n.c & 0xFFFF) << 32, size = n.b | (uint64_t)(n.c & 0xFFFF0000) << 16;
            if (offset + size > (uint64_t)blocks_.size() * kBlock) return err = "corrupt archive (" + path + " out of range)", false;
            files_.push_back({path, offset, size});
        } else if (!list(i, path, depth + 1, err)) {
            return false;
        }
    }
    return true;
}

bool Archive::open(int fd, std::string& err) {
    fd_ = fd;
    Footer f;
    if (!read_footer(fd, f)) return err = "not a Wii U archive (.wua), or a damaged one", false;
    dataOffset_ = f.off[0];
    dataSize_ = f.size[0];
    if (f.size[1] % kRecordSize || f.size[1] > 0xFFFFFFFFu || f.size[2] > 0x7FFFFFFFu || f.size[3] % 16 || f.size[3] > 0xFFFFFFFFu ||
        f.size[1] == 0 || f.size[3] == 0)
        return err = "corrupt archive (header)", false;
    // compressed block table: every record holds a full offset and the sizes - 1 of 16 blocks
    std::vector<uint8_t> rec(f.size[1]);
    if (!pread_all(fd, f.off[1], rec.data(), rec.size())) return err = "cannot read the archive", false;
    blocks_.clear();
    blocks_.reserve(rec.size() / kRecordSize * kBlocksPerRecord);
    for (size_t r = 0; r < rec.size(); r += kRecordSize) {
        uint64_t off = be64(rec.data() + r);
        for (uint32_t i = 0; i < kBlocksPerRecord; i++) {
            uint32_t size = (uint32_t)be16(rec.data() + r + 8 + 2 * i) + 1;
            blocks_.push_back({off, size});
            off += size;
        }
    }
    names_.resize(f.size[2]);
    if (!pread_all(fd, f.off[2], names_.data(), names_.size())) return err = "cannot read the archive", false;
    std::vector<uint8_t> tree(f.size[3]);
    if (!pread_all(fd, f.off[3], tree.data(), tree.size())) return err = "cannot read the archive", false;
    nodes_.resize(tree.size() / 16);
    for (size_t i = 0; i < nodes_.size(); i++) {
        const uint8_t* p = tree.data() + i * 16;
        nodes_[i] = {be32(p), be32(p + 4), be32(p + 8), be32(p + 12)};
    }
    if (nodes_[0].nameAndType & 0x80000000) return err = "corrupt archive (no root folder)", false;

    // the base game's folder: Cemu stores each title as "<title id>_v<version>" at the top, the
    // game (0005000 0...) next to its update (...e...) and DLC (...c...)
    const Node& root = nodes_[0];
    if ((uint64_t)root.a + root.b > nodes_.size()) return err = "corrupt archive (file tree)", false;
    int64_t game = -1;
    bool other = false, hasCode = false;
    for (uint32_t i = root.a; i < root.a + root.b; i++) {
        if (nodes_[i].nameAndType & 0x80000000) continue;
        std::string nm = name(nodes_[i]);
        if (iequals(nm, "code")) hasCode = true;  // a single title stored without its folder
        std::string t = title_of(nm);
        if (t.empty()) continue;
        if (t.compare(0, 8, "00050000") != 0) {
            other = true;
            continue;
        }
        if (game < 0) {
            game = i;
            titleId_ = t;
        }
    }
    files_.clear();
    if (game >= 0) {
        if (!list((uint32_t)game, "", 0, err)) return false;
    } else if (hasCode) {
        if (!list(0, "", 0, err)) return false;
    } else {
        return err = other ? "this archive holds only an update or DLC, not the game itself" : "no Wii U game in this archive", false;
    }
    bool rpx = false;
    for (auto& fl : files_)
        if (fl.path.rfind("code/", 0) == 0 && fl.path.size() > 4 && fl.path.compare(fl.path.size() - 4, 4, ".rpx") == 0) rpx = true;
    if (!rpx) return err = "no game executable (code/*.rpx) in this archive", false;
    return true;
}

bool Archive::read_block(uint64_t index, uint8_t* out, std::vector<uint8_t>& scratch, void* dctx) const {
    if (index >= blocks_.size()) return false;
    auto [off, size] = blocks_[index];
    if (off > dataSize_ || size > dataSize_ - off) return false;
    if (size == kBlock) return pread_all(fd_, dataOffset_ + off, out, kBlock);  // stored uncompressed
    scratch.resize(size);
    if (!pread_all(fd_, dataOffset_ + off, scratch.data(), size)) return false;
    size_t n = ZSTD_decompressDCtx((ZSTD_DCtx*)dctx, out, kBlock, scratch.data(), size);
    return !ZSTD_isError(n) && n == kBlock;
}

bool Archive::extract(const std::string& outDir, const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress,
                      std::string& err) {
    // as disc::Image::extract: files in parallel, big ones first, progress from one thread at a time
    std::vector<const File*> todo;
    uint64_t total = 0;
    for (auto& fl : files_) {
        todo.push_back(&fl);
        total += fl.size;
    }
    std::sort(todo.begin(), todo.end(), [](const File* a, const File* b) { return a->size > b->size; });
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> done{0};
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::string firstErr;
    auto report = [&](const std::string& name) {
        std::lock_guard<std::mutex> lk(mu);
        if (progress && !progress(done.load(), total, name)) stop = true;
    };
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(mu);
        if (firstErr.empty()) firstErr = why;
        stop = true;
    };
    auto worker = [&] {
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        if (!dctx) return fail("out of memory");
        std::vector<uint8_t> block(kBlock), scratch;
        uint64_t cached = UINT64_MAX;  // the block in `block` (files share their first and last blocks)
        for (size_t i; !stop && (i = next++) < todo.size();) {
            const File& fl = *todo[i];
            std::string dst = outDir + "/" + fl.path;
            if (!mkdirs(dst.substr(0, dst.find_last_of('/')))) {
                fail("cannot create a folder for " + fl.path);
                break;
            }
            std::string tmp = dst + ".part";
            FILE* out = fopen(tmp.c_str(), "wb");
            if (!out) {
                fail("cannot write " + fl.path);
                break;
            }
            std::string ferr;
            uint64_t pos = fl.offset, remaining = fl.size, sinceReport = 0;
            while (remaining > 0 && !stop) {
                uint64_t b = pos / kBlock, within = pos % kBlock;
                if (b != cached) {
                    cached = UINT64_MAX;
                    if (!read_block(b, block.data(), scratch, dctx)) {
                        ferr = "read error in " + fl.path + " (damaged archive?)";
                        break;
                    }
                    cached = b;
                }
                size_t take = (size_t)std::min<uint64_t>(remaining, kBlock - within);
                if (fwrite(block.data() + within, 1, take, out) != take) {
                    ferr = "cannot write " + fl.path + " (storage full?)";
                    break;
                }
                pos += take;
                remaining -= take;
                done += take;
                if ((sinceReport += take) >= (4u << 20)) {
                    sinceReport = 0;
                    report(fl.path);
                }
            }
            bool ok = (fclose(out) == 0) && remaining == 0 && ferr.empty();
            if (!ok) {
                remove(tmp.c_str());
                if (!ferr.empty()) fail(ferr);
                else if (!stop) fail("cannot write " + fl.path + " (storage full?)");
                break;
            }
            if (rename(tmp.c_str(), dst.c_str()) != 0) {
                fail("cannot write " + fl.path);
                break;
            }
            report(fl.path);
        }
        ZSTD_freeDCtx(dctx);
    };
    unsigned n = std::max(1u, std::min(6u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; t++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (stop) {
        err = firstErr.empty() ? "cancelled" : firstErr;
        return false;
    }
    if (progress) progress(total, total, "");
    return true;
}

}  // namespace disc
