#include "archive/AssetPack.hpp"

#include "core/util/AssetPaths.hpp"
#include "core/util/Logger.hpp"

#include <lzma.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace adofai {
namespace archive {

namespace {

struct Entry {
    size_t offset = 0;   // 在解压后的 tar 流里的偏移
    size_t size = 0;
};

struct Pack {
    bool tried = false;
    bool ok = false;
    std::string path;
    std::vector<uint8_t> bytes;                      // 解压后的 tar（原样）
    std::unordered_map<std::string, Entry> entries;  // 条目名 → 位置
};

Pack g_pack;
std::once_flag g_once;

bool readWhole(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    out.resize((size_t)n);
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return got == (size_t)n;
}

// 整份解压（auto = 同时接受 .xz 与老的 .lzma alone；`ilytx` 那条路用的是流式，这里图简单）。
bool inflateXz(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_auto_decoder(&strm, UINT64_MAX, 0) != LZMA_OK) {
        LOG_E("AssetPack: lzma_auto_decoder 初始化失败");
        return false;
    }
    strm.next_in = data;
    strm.avail_in = size;
    out.assign(1u << 20, 0);
    size_t used = 0;
    bool ok = false;
    for (;;) {
        if (used == out.size()) out.resize(out.size() * 2);
        strm.next_out = out.data() + used;
        strm.avail_out = out.size() - used;
        const lzma_ret ret = lzma_code(&strm, LZMA_FINISH);
        used = out.size() - strm.avail_out;
        if (ret == LZMA_STREAM_END) { ok = true; break; }
        if (ret != LZMA_OK) {
            LOG_E("AssetPack: 解压失败（lzma_ret=%d）", (int)ret);
            break;
        }
    }
    lzma_end(&strm);
    out.resize(used);
    return ok;
}

// tar 的 size 字段是 11 位八进制 + NUL/空格；GNU 的 base-256 也认一下（大文件/怪异 tar）。
uint64_t parseTarSize(const uint8_t* p) {
    if (p[0] & 0x80) {   // base-256
        uint64_t v = p[0] & 0x7F;
        for (int i = 1; i < 12; ++i) v = (v << 8) | p[i];
        return v;
    }
    uint64_t v = 0;
    for (int i = 0; i < 12; ++i) {
        const uint8_t c = p[i];
        if (c == 0 || c == ' ') break;
        if (c < '0' || c > '7') break;
        v = v * 8 + (uint64_t)(c - '0');
    }
    return v;
}

std::string field(const uint8_t* p, size_t maxLen) {
    size_t n = 0;
    while (n < maxLen && p[n] != 0) ++n;
    return std::string((const char*)p, n);
}

void buildIndex(Pack& pack) {
    const std::vector<uint8_t>& b = pack.bytes;
    size_t pos = 0;
    std::string longName;   // GNU 'L' 的负载
    while (pos + 512 <= b.size()) {
        const uint8_t* h = b.data() + pos;
        // 全零块 = 结束
        bool zero = true;
        for (int i = 0; i < 512; ++i) if (h[i]) { zero = false; break; }
        if (zero) break;

        std::string name = field(h, 100);
        const uint64_t size = parseTarSize(h + 124);
        const char type = (char)h[156];
        // ustar 的 prefix（偏移 345，155 字节）
        if (h[257] == 'u' && h[258] == 's' && h[259] == 't' && h[260] == 'a' && h[261] == 'r') {
            const std::string prefix = field(h + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        const size_t dataOff = pos + 512;
        if (dataOff + size > b.size()) break;
        pos = dataOff + ((size + 511) / 512) * 512;

        if (type == 'L') {   // GNU longname：下一块的 name 用它
            const char* lp = (const char*)b.data() + dataOff;
            size_t ln = 0;
            while (ln < (size_t)size && lp[ln]) ++ln;
            longName.assign(lp, ln);
            continue;
        }
        if (!longName.empty()) { name = longName; longName.clear(); }
        if (type == '0' || type == '\0') {
            pack.entries[name] = Entry{dataOff, (size_t)size};
        }
    }
    LOG_I("AssetPack: %s 载入 %zu 个条目（%.1f MB 解压后）", pack.path.c_str(), pack.entries.size(),
          (double)pack.bytes.size() / (1024.0 * 1024.0));
}

void loadOnce() {
    Pack& pack = g_pack;
    pack.tried = true;
    const AssetOptions& opts = assetOptions();
    if (opts.packName.empty()) return;
    pack.path = resolveAsset(opts.packName);
    std::vector<uint8_t> raw;
    if (!readWhole(pack.path, raw)) {
        LOG_D("AssetPack: 没有 %s（走磁盘资产）", pack.path.c_str());
        return;
    }
    if (!inflateXz(raw.data(), raw.size(), pack.bytes)) return;
    buildIndex(pack);
    pack.ok = !pack.entries.empty();
}

}  // namespace

bool readAssetFromPack(const char* name, std::vector<uint8_t>& out) {
    if (!name || !*name) return false;
    std::call_once(g_once, loadOnce);
    const Pack& pack = g_pack;
    if (!pack.ok) return false;
    auto it = pack.entries.find(name);
    if (it == pack.entries.end()) return false;
    const Entry& e = it->second;
    out.assign(pack.bytes.begin() + (std::ptrdiff_t)e.offset,
               pack.bytes.begin() + (std::ptrdiff_t)(e.offset + e.size));
    return true;
}

}  // namespace archive
}  // namespace adofai
