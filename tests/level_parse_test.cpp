// 关卡解析对拍测试：快路径必须与 cleanJson + RapidJSON 老路径逐位一致。
//
// LevelData::tryFastParse() 绕过了 cleanJson()，直接在 mmap 出来的原文上扫描，所以它
// 必须逐条复现 cleanJson() 的容错规则。level_fixtures/ 里每个固定用例对应一条规则
// （生成脚本见 gen_level_fixtures.py）：
//   * 漏写逗号：值后面直接跟 " / { / [        （老编辑器/老版本会这么写）
//   * 前置逗号 / 重复逗号 / 尾随逗号           （丢掉）
//   * CRLF 行尾、字符串里的裸 CR              （丢掉）
//   * 数字/字符串/布尔/整数各变体、BOM、只有 pathData、空数组、最小文件 …
//   * 压缩容器 .adofai.xz / .adofai.zst：明文 fixture 在内存里压一遍再加载，必须逐位一致
//   * actions 分块并行解析（ADOCAO_PARSE_PIECES=4 强制）：与顺序解析必须逐位一致
//   * 流式解压的乒乓半窗（4 KB）：按序拼回来必须与整份解压逐字节相同，超大单值要能报卡死
//
// 每个用例加载两遍：一遍强制走老路径（ADOCAO_FORCE_DOM_PARSE=1），一遍走快路径，
// 然后按节比较 LevelData（angleData / actions / settings / tiles / 每条派生数组），
// 逐位相同才算通过。快路径还会再加载一遍，确认自身可重复（并行/缓存不得引入抖动）。
//
// 用法：
//   adocao_level_parse_test <目录或文件> ...        目录会展开成其中的 *.json
// 也可以直接喂真实谱面，例如：
//   adocao_level_parse_test ~/Documents/Charts/**/*.adofai

#include <fstream>
#include "core/level/LevelPath.hpp"
#include "archive/LevelArchive.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <lzma.h>
#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace fs = std::filesystem;

// ---------------------------------------------------------------- digest
namespace {

struct H {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = (const unsigned char*)p;
        for (size_t i = 0; i < n; i++) { h ^= c[i]; h *= 1099511628211ull; }
    }
    void u64(uint64_t v) { bytes(&v, 8); }
    void i32(int v) { bytes(&v, 4); }
    void f32(float v) { bytes(&v, 4); }
    void str(const std::string& s) { u64(s.size()); if (!s.empty()) bytes(s.data(), s.size()); }
    uint64_t operator()() const { return h; }
};

struct Digest {
    uint64_t angle = 0, actions = 0, tiles = 0, settings = 0, bpm = 0, twirl = 0, setspeed = 0;
    uint64_t bookmarks = 0, hitsounds = 0, hsVolumes = 0, posOffsets = 0, atStates = 0, path = 0;
    size_t angleCount = 0, actionCount = 0, tileCount = 0, bookmarkCount = 0, hitsoundCount = 0;
    size_t hsVolumeCount = 0, posOffsetCount = 0, atStateCount = 0, pathLen = 0;
    bool ok = false;
};

template <typename V>
void hashMap(const V& m, H& hh) {
    std::vector<int> keys;
    keys.reserve(m.size());
    for (auto& kv : m) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (int k : keys) {
        hh.i32(k);
        const auto& v = m.at(k);
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) hh.str(v);
        else if constexpr (std::is_floating_point_v<std::decay_t<decltype(v)>>) hh.f32(v);
        else { hh.f32(v.offsetX); hh.f32(v.offsetY); hh.bytes(&v.justThisTile, 1); }
    }
}

Digest digest(const LevelData& lv, bool ok) {
    Digest d;
    d.ok = ok;
    if (!ok) return d;
    H a;  for (double v : lv.angleData) a.bytes(&v, 8);
    H ac; for (auto& x : lv.actions) {
        ac.i32(x.floor); ac.u64(x.type); ac.f32(x.val1); ac.f32(x.val2);
        ac.bytes(&x.flag, 1); ac.str(lv.actionStr(x));
    }
    H t;  for (auto& x : lv.tiles) {
        // Tile::index 已删（层号 = tiles 里的位置，从来没有读者）—— 摘要里也不再哈希它
        t.f32(x.angle); t.f32(x.direction); t.bytes(&x.position, sizeof(x.position));
    }
    H s;
    s.i32(lv.settings.version); s.f32(lv.settings.bpm); s.f32(lv.settings.offset);
    s.i32(lv.settings.countdownTicks); s.f32(lv.settings.zoom); s.f32(lv.settings.rotation);
    s.str(lv.settings.relativeTo); s.bytes(&lv.settings.position, sizeof(lv.settings.position));
    s.str(lv.settings.hitsound); s.f32(lv.settings.hitsoundVolume); s.str(lv.settings.trackColor);
    s.str(lv.settings.secondaryTrackColor); s.str(lv.settings.backgroundColor);
    s.bytes(&lv.settings.stickToFloors, 1); s.str(lv.settings.planetEase);
    s.str(lv.settings.trackDisappearAnimation); s.str(lv.settings.trackAnimation);
    s.f32(lv.settings.beatsBehind); s.f32(lv.settings.beatsAhead);
    H bp; for (float v : lv.tileBPMs) bp.f32(v);
    H tw; for (size_t i = 0; i < lv.tileHasTwirl.size(); i++)    { unsigned char b = lv.tileHasTwirl[i];    tw.bytes(&b, 1); }
    H ss; for (size_t i = 0; i < lv.tileHasSetSpeed.size(); i++) { unsigned char b = lv.tileHasSetSpeed[i]; ss.bytes(&b, 1); }
    H bm; for (int v : lv.bookmarkFloors) bm.i32(v);
    H hs; hashMap(lv.tileHitsounds, hs);
    H hv; size_t hvCount = 0;
    for (size_t i = 0; i < lv.tileHitsoundVolumes.size(); i++) {   // 稠密数组：NaN = 无覆盖
        if (std::isnan(lv.tileHitsoundVolumes[i])) continue;
        hv.u64(i); hv.f32(lv.tileHitsoundVolumes[i]); hvCount++;
    }
    H po; hashMap(lv.tilePositionOffsets, po);
    H at;
    {
        std::vector<int> keys;
        for (auto& kv : lv.atStates) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (int k : keys) {
            const auto& v = lv.atStates.at(k);
            at.i32(k); at.str(v.da); at.str(v.aa); at.f32(v.bb); at.f32(v.ba); at.bytes(&v.hasAA, 1);
        }
    }
    H p; p.str(lv.pathData);

    d.angle = a();    d.angleCount = lv.angleData.size();
    d.actions = ac(); d.actionCount = lv.actions.size();
    d.tiles = t();    d.tileCount = lv.tiles.size();
    d.settings = s(); d.bpm = bp(); d.twirl = tw(); d.setspeed = ss();
    d.bookmarks = bm(); d.bookmarkCount = lv.bookmarkFloors.size();
    d.hitsounds = hs(); d.hitsoundCount = lv.tileHitsounds.size();
    d.hsVolumes = hv(); d.hsVolumeCount = hvCount;
    d.posOffsets = po(); d.posOffsetCount = lv.tilePositionOffsets.size();
    d.atStates = at();  d.atStateCount = lv.atStates.size();
    d.path = p();       d.pathLen = lv.pathData.size();
    return d;
}

struct Section { const char* name; uint64_t Digest::*hash; size_t Digest::*count; };

const Section kSections[] = {
    {"angleData",           &Digest::angle,      &Digest::angleCount},
    {"actions",             &Digest::actions,    &Digest::actionCount},
    {"tiles",               &Digest::tiles,      &Digest::tileCount},
    {"settings",            &Digest::settings,   nullptr},
    {"tileBPMs",            &Digest::bpm,        nullptr},
    {"tileHasTwirl",        &Digest::twirl,      nullptr},
    {"tileHasSetSpeed",     &Digest::setspeed,   nullptr},
    {"bookmarkFloors",      &Digest::bookmarks,  &Digest::bookmarkCount},
    {"tileHitsounds",       &Digest::hitsounds,  &Digest::hitsoundCount},
    {"tileHitsoundVolumes", &Digest::hsVolumes,  &Digest::hsVolumeCount},
    {"tilePositionOffsets", &Digest::posOffsets, &Digest::posOffsetCount},
    {"atStates",            &Digest::atStates,   &Digest::atStateCount},
    {"pathData",            &Digest::path,       &Digest::pathLen},
};

// 返回不同的节名（空 = 完全一致）
std::string diffSections(const Digest& x, const Digest& y) {
    if (x.ok != y.ok) return x.ok ? "ok(快路径成功/老路径失败)" : "ok(老路径成功/快路径失败)";
    if (!x.ok) return {};
    std::string out;
    for (auto& s : kSections) {
        bool differing = (x.*s.hash) != (y.*s.hash);
        if (s.count && (x.*s.count) != (y.*s.count)) differing = true;
        if (differing) { if (!out.empty()) out += ", "; out += s.name; }
    }
    return out;
}

// setenv/unsetenv 是 POSIX 的，Windows（MinGW）只有 _putenv_s（它才会更新 CRT 自己的
// environ，SetEnvironmentVariable 不会）
void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

std::string readRawFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string out(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0 && std::fread(&out[0], 1, (size_t)n, f) != (size_t)n) out.clear();
    std::fclose(f);
    return out;
}

bool compressXz(const std::string& in, std::string& out) {
    out.resize(lzma_stream_buffer_bound(in.size()));
    size_t pos = 0;
    lzma_ret r = lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, nullptr,
                                         (const uint8_t*)in.data(), in.size(),
                                         (uint8_t*)out.data(), &pos, out.size());
    if (r != LZMA_OK) return false;
    out.resize(pos);
    return true;
}

bool compressZstd(const std::string& in, std::string& out) {
    out.resize(ZSTD_compressBound(in.size()));
    size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 19);
    if (ZSTD_isError(n)) return false;
    out.resize(n);
    return true;
}


// 流式解压（乒乓半窗）的字节级校验：用 4 KB 的半窗读压缩流，消费方每块只吃 2/3、
// 剩下 1/3 当"残缺值"留给下一块。把吃掉的字节按序拼起来，必须与整份解压逐字节相同。
// leave: 每块故意留下的残字节数。realistic 消费方在流结束时会把剩下的全吃掉。
std::string windowedCopy(const std::string& packed, LevelArchiveKind kind, size_t leaveTail) {
    ArchiveStream st;
    if (!st.open(packed.data(), packed.size(), kind, 4096)) return "<open-failed>";
    std::string out;
    while (st.next()) {
        const size_t n = st.size();
        const size_t complete = (st.eof() || n <= leaveTail) ? n : n - leaveTail;
        out.append(st.data(), complete);
        st.consume(complete);
    }
    if (st.failed()) return "<failed>";
    if (st.stuck()) return "<stuck>";
    return out;
}

// 半窗机制的整体自检：256 KB 伪数据（含 JSON 特殊字符）→ 压成 xz/zstd → 4 KB 半窗读回来。
// 覆盖多块、carry、以及"单个值比窗口大"的卡死检测。返回空串表示通过。
std::string streamSelfTest() {
    std::string payload;
    payload.reserve(256 * 1024);
    uint32_t x = 0x12345678u;                    // xorshift：造不可压的伪随机字节
    for (size_t i = 0; i < 256 * 1024; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        payload.push_back((char)(x >> 24));
    }
    for (int kind = 0; kind < 2; kind++) {
        const LevelArchiveKind k = kind ? LevelArchiveKind::Zstd : LevelArchiveKind::Xz;
        const char* name = kind ? "zstd" : "xz";
        std::string packed;
        const bool ok = kind ? compressZstd(payload, packed) : compressXz(payload, packed);
        if (!ok) return std::string(name) + " 压缩失败";
        if (packed.size() < 4096 * 4) return std::string(name) + " 测试数据不够跨多块";
        // 每块全部吃掉（真实消费方在值边界上的极限情况）
        if (windowedCopy(packed, k, 0) != payload) return std::string(name) + " 全吃模式下拼接不一致";
        // 每块留 1 字节当残缺值（反复 carry）
        if (windowedCopy(packed, k, 1) != payload) return std::string(name) + " 留 1 字节 carry 后拼接不一致";
        // 每块留 1KB（跨多块才凑齐一个值）
        if (windowedCopy(packed, k, 1024) != payload) return std::string(name) + " 留 1KB carry 后拼接不一致";
        // 一点都吃不下 -> 必须报 stuck，而不是死循环
        ArchiveStream st;
        if (!st.open(packed.data(), packed.size(), k, 4096)) return std::string(name) + " 打开失败";
        if (!st.next()) return std::string(name) + " 第一块就失败";
        st.consume(0);
        if (st.next() || !st.stuck()) return std::string(name) + " 超大单值没有报 stuck";
    }
    return {};
}


Digest loadBuffer(const char* data, size_t length);   // 定义在后面

// 「选中文件夹也能加载」的解析规则：唯一命中才生效，含糊（0 个/多个）必须原样返回。
std::string levelPathResolveSelfTest() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "adocao_levelpath_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root / "one", ec);
    fs::create_directories(root / "many", ec);
    fs::create_directories(root / "nested" / "chart", ec);
    std::ofstream(root / "plain.adofai").put('x');
    std::ofstream(root / "one" / "a.adofai").put('x');
    std::ofstream(root / "many" / "a.adofai").put('x');
    std::ofstream(root / "many" / "b.adofai.xz").put('x');
    std::ofstream(root / "nested" / "chart" / "c.adofai.zst").put('x');
    struct Case { fs::path in; fs::path want; const char* what; };
    const Case cases[] = {
        { root / "plain.adofai", root / "plain.adofai",                      "文件原样返回" },
        { root / "one",          root / "one" / "a.adofai",                  "目录里唯一的谱" },
        { root / "nested",       root / "nested" / "chart" / "c.adofai.zst", "往下看一层子目录" },
        { root / "many",         root / "many",                              "多个命中不猜" },
        { root / "missing",      root / "missing",                           "不存在原样返回" },
    };
    for (const auto& c : cases) {
        const std::string got = resolveLevelPath(c.in.string());
        if (got != c.want.string())
            return std::string(c.what) + "：期望 " + c.want.string() + "，得到 " + got;
    }
    // 候选列表：多个要能列全（供 UI 提示用）
    const auto many = listLevelCandidates((root / "many").string());
    if (many.size() != 2) return "候选列表：期望 2 个，得到 " + std::to_string(many.size());
    if (listLevelCandidates((root / "one").string()).size() != 1) return "候选列表：单谱目录应为 1";
    if (!listLevelCandidates((root / "plain.adofai").string()).empty()) return "候选列表：文件应为空";
    fs::remove_all(root, ec);
    return {};
}


// 直接语义断言（不是两路对拍）：负音量/零音量必须原样落到 tileHitsoundVolumes。
// 这条正是被 Unity.wav_rate 暴露的盲区 —— 当时两条解析路径都把负值当成"没写音量"，
// 于是对拍摘要永远一致，而声音全错。
std::string rawVolumeSemanticsSelfTest() {
    const char* text =
        "{\"angleData\": [0, 90, 180, 0], \"settings\": {\"bpm\": 120, \"hitsoundVolume\": 100}, "
        "\"actions\": ["
        "{ \"floor\": 1, \"eventType\": \"SetHitsound\", \"hitsoundVolume\": -33.5 }, "
        "{ \"floor\": 2, \"eventType\": \"SetHitsound\", \"hitsoundVolume\": -0.5 }, "
        "{ \"floor\": 3, \"eventType\": \"SetHitsound\", \"hitsoundVolume\": 0 }, "
        "{ \"floor\": 4, \"eventType\": \"SetHitsound\", \"hitsoundVolume\": 12.25 }, "
        "{ \"floor\": 5, \"eventType\": \"SetHitsound\" }], \"decorations\": []}";
    const char* expectFile = "负音量语义";
    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setEnv("ADOCAO_FORCE_DOM_PARSE", "1");
        LevelData lv;
        const bool ok = lv.loadFromBuffer(text, std::strlen(text));
        if (legacy) unsetEnv("ADOCAO_FORCE_DOM_PARSE");
        if (!ok) return std::string(expectFile) + (legacy ? "（旧路径）加载失败" : "（快路径）加载失败");
        const float want[6] = {0.f, -33.5f, -0.5f, 0.f, 12.25f, 100.f};
        for (int f = 1; f <= 5; f++) {
            const float got = lv.hasHitsoundVolume(f) ? lv.tileHitsoundVolumes[(size_t)f] : 100.0f;
            if (std::fabs(got - want[f]) > 1e-4f) {
                char buf[160];
                snprintf(buf, sizeof buf, "%s floor %d: 期望 %.2f，实际 %.2f（%s）",
                         expectFile, f, want[f], got, legacy ? "旧路径" : "快路径");
                return buf;
            }
        }
    }
    return {};
}

// TrackVis（trackDisappearAnimation / AnimateTrack）：Timeline 里那两个 16 B/层的数组现在按需分配。
// 判据必须与消费方 app/LevelScene.cpp 的 `m_tileVisEnabled` **逐字相同** —— 否则要么"数组空但消费方
// 以为启用"（越界读），要么"数组在但消费方不用"（白占 16 B/层，1e9 层就是 16 GB）。
// 这个用例把等价性钉死：`数组非空` ⟺ `消费方判据`，三种谱各查一遍，两条解析路径都查。
std::string trackVisAllocationSelfTest() {
    struct Case { const char* name; const char* text; bool want; };
    const Case cases[] = {
        {"无动画事件", "{\"angleData\":[0,90,180,0],\"settings\":{\"bpm\":120},\"actions\":[],\"decorations\":[]}", false},
        {"有 AnimateTrack",
         "{\"angleData\":[0,90,180,0],\"settings\":{\"bpm\":120},\"actions\":["
         "{\"floor\":1,\"eventType\":\"AnimateTrack\",\"trackDisappearAnimation\":\"Disappear\"}],\"decorations\":[]}", true},
        {"settings 开了动画",
         "{\"angleData\":[0,90,180,0],\"settings\":{\"bpm\":120,\"trackDisappearAnimation\":\"Disappear\"},"
         "\"actions\":[],\"decorations\":[]}", true},
    };
    for (const auto& c : cases) {
        for (int legacy = 0; legacy < 2; legacy++) {
            if (legacy) setEnv("ADOCAO_FORCE_DOM_PARSE", "1");
            LevelData lv;
            const bool ok = lv.loadFromBuffer(c.text, std::strlen(c.text));
            if (legacy) unsetEnv("ADOCAO_FORCE_DOM_PARSE");
            const char* which = legacy ? "（旧路径）" : "（快路径）";
            if (!ok) return std::string(c.name) + which + "加载失败";

            Timeline tl;
            tl.build(lv);
            const bool allocated = !tl.tileDisappearTimes().empty() || !tl.tileAppearTimes().empty();
            // 逐字复制 app/LevelScene.cpp 的 m_tileVisEnabled
            const bool consumerEnabled = (lv.settings.trackDisappearAnimation != "None" ||
                                          lv.settings.trackAnimation != "None" ||
                                          !lv.atStates.empty());
            if (allocated != consumerEnabled) {
                char buf[224];
                std::snprintf(buf, sizeof buf,
                              "TrackVis 分配判据与消费方不一致（%s）：数组%s但消费方认为%s",
                              c.name, allocated ? "已分配" : "为空", consumerEnabled ? "启用" : "禁用");
                return std::string(buf) + which;
            }
            if (consumerEnabled != c.want) {   // 顺带证明用例本身有牙（预期写错就会红）
                char buf[224];
                std::snprintf(buf, sizeof buf, "用例预期与消费方判据不符（%s）：预期 %s，实际 %s",
                              c.name, c.want ? "启用" : "禁用", consumerEnabled ? "启用" : "禁用");
                return std::string(buf) + which;
            }
        }
    }
    return {};
}

// 单个值远大于半窗：`levelDesc` 是 200 KB 的字符串、每个装饰物里还有 300 B 的字符串。
// skip / settings / path 三个子扫描器带状态可续，必须能跨窗流过去，而不是"放弃并回退"。
std::string windowHugeValueSelfTest() {
    std::string text = "{\"angleData\":[0, 90, 180], \"levelDesc\":\"";
    text.append(200 * 1024, 'x');
    text += "\", \"settings\": {\"bpm\": 100}, \"decorations\": [";
    for (int i = 0; i < 500; i++) {
        if (i) text += ',';
        text += "{\"floor\": " + std::to_string(i) + ", \"eventType\": \"AddDecoration\", \"decorationImage\": \"";
        text.append(300, 'd');
        text += "\"}";
    }
    text += "], \"actions\": [{ \"floor\": 1, \"eventType\": \"Twirl\" }]}";

    std::string packed;
    if (!compressXz(text, packed)) return "xz 压缩失败";
    setEnv("ADOCAO_WINDOW_KB", "4");
    setEnv("ADOCAO_WINDOW_REQUIRE", "1");
    const Digest win = loadBuffer(packed.data(), packed.size());
    unsetEnv("ADOCAO_WINDOW_REQUIRE");
    unsetEnv("ADOCAO_WINDOW_KB");
    setEnv("ADOCAO_WHOLE_DECOMPRESS", "1");
    const Digest whole = loadBuffer(packed.data(), packed.size());
    unsetEnv("ADOCAO_WHOLE_DECOMPRESS");
    if (!win.ok || !whole.ok) return "加载失败（win/whole）";
    const std::string d = diffSections(whole, win);
    return d.empty() ? std::string() : ("超大单值跨窗不一致: " + d);
}



// 窗口边界的压力用例：合成一张远大于窗口的谱（action 对象故意跨窗），走窗口路径加载，
// 最终结果必须与整份解压逐位一致。fixture 都小于窗口、压不到边界，所以单独造一个。
// 目前窗口路径在这个规模上会"放弃并回退"（放弃是安全失败），这条用例保证回退后的结果
// 依然正确；等窗口路径自身修好，它会自动开始真正覆盖边界逻辑。
std::string windowBoundarySelfTest() {
    std::string text = "{\"angleData\":[";
    for (int i = 0; i < 3000; i++) { if (i) text += ','; text += (i % 4 == 0) ? "90" : "180"; }
    text += "],\"settings\":{\"bpm\":100},\"actions\":[";
    char buf[256];
    for (int i = 0; i < 2000; i++) {
        if (i) text += ',';
        int n = snprintf(buf, sizeof buf,
            "{ \"floor\": %d, \"eventType\": \"SetHitsound\", \"hitsound\": \"Kick\", "
            "\"hitsoundVolume\": 100, \"angleOffset\": 0, \"relativeTo\": [0,\"ThisTile\"] }", i);
        text.append(buf, n);
    }
    text += "],\"decorations\":[]}";
    std::string packed;
    if (!compressXz(text, packed)) return "xz 压缩失败";
    setEnv("ADOCAO_WINDOW_KB", "4");
    const Digest win = loadBuffer(packed.data(), packed.size());
    unsetEnv("ADOCAO_WINDOW_KB");
    setEnv("ADOCAO_WHOLE_DECOMPRESS", "1");
    const Digest whole = loadBuffer(packed.data(), packed.size());
    unsetEnv("ADOCAO_WHOLE_DECOMPRESS");
    if (!win.ok || !whole.ok) return "加载失败（win/whole）";
    const std::string d = diffSections(whole, win);
    if (!d.empty()) return "窗口路径与整份结果不一致: " + d;
    return {};
}

Digest loadBuffer(const char* data, size_t length) {
    LevelData lv;
    bool ok = lv.loadFromBuffer(data, length);
    return digest(lv, ok);
}

// 压缩容器往返：把明文谱在内存里压成 .xz / .zst 再喂回去，必须和明文加载
// 逐位一致。全部在内存里做，不落任何临时文件（大字面量谱面跳过，压一轮太贵）。
std::string archiveRoundTrip(const std::string& file, const Digest& plain) {
    const std::string raw = readRawFile(file);
    if (raw.empty()) return {};
    if (sniffLevelArchive(raw.data(), raw.size()) != LevelArchiveKind::Plain) return {};  // 本身就是容器
    if (raw.size() > (8u << 20)) return {};                                              // 太大，跳过

    // 只有当"整份快路径"自己能吃下这个 fixture 时，才要求窗口路径也必须吃下
    // （f17/f23/f31 这类本来就该回退旧路径的用例，两条路都可以放弃）
    setEnv("ADOCAO_FAST_REQUIRE", "1");
    LevelData probe;
    const bool fastHandles = probe.loadFromBuffer(raw.data(), raw.size());
    unsetEnv("ADOCAO_FAST_REQUIRE");

    std::string packed;
    if (compressXz(raw, packed)) {
        if (windowedCopy(packed, LevelArchiveKind::Xz, 1) != raw) return "xz 半窗流式解压与整份不一致";
        if (fastHandles) setEnv("ADOCAO_WINDOW_REQUIRE", "1");   // 快路径能吃下 -> 窗口也必须能
        setEnv("ADOCAO_WINDOW_KB", "4");                  // 强制走窗口路径，且窗口极小
        const std::string d = diffSections(plain, loadBuffer(packed.data(), packed.size()));
        unsetEnv("ADOCAO_WINDOW_KB");
        unsetEnv("ADOCAO_WINDOW_REQUIRE");
        if (!d.empty()) return "xz 窗口路径: " + d;
        setEnv("ADOCAO_WHOLE_DECOMPRESS", "1");            // 同一条流走整份解压做对照
        const std::string d2 = diffSections(plain, loadBuffer(packed.data(), packed.size()));
        unsetEnv("ADOCAO_WHOLE_DECOMPRESS");
        if (!d2.empty()) return "xz 整份路径: " + d2;
        // 截断的 xz 必须干净地失败，而不是崩
        if (loadBuffer(packed.data(), packed.size() / 2).ok) return "截断的 xz 竟然加载成功";
    }
    if (compressZstd(raw, packed)) {
        if (windowedCopy(packed, LevelArchiveKind::Zstd, 1) != raw) return "zstd 半窗流式解压与整份不一致";
        if (fastHandles) setEnv("ADOCAO_WINDOW_REQUIRE", "1");   // 快路径能吃下 -> 窗口也必须能
        setEnv("ADOCAO_WINDOW_KB", "4");
        const std::string d = diffSections(plain, loadBuffer(packed.data(), packed.size()));
        unsetEnv("ADOCAO_WINDOW_KB");
        unsetEnv("ADOCAO_WINDOW_REQUIRE");
        if (!d.empty()) return "zstd 窗口路径: " + d;
        setEnv("ADOCAO_WHOLE_DECOMPRESS", "1");
        const std::string d2 = diffSections(plain, loadBuffer(packed.data(), packed.size()));
        unsetEnv("ADOCAO_WHOLE_DECOMPRESS");
        if (!d2.empty()) return "zstd 整份路径: " + d2;
        if (loadBuffer(packed.data(), packed.size() / 2).ok) return "截断的 zstd 竟然加载成功";
    }
    return {};
}

Digest loadPath(const std::string& file, bool legacy) {
    if (legacy) setEnv("ADOCAO_FORCE_DOM_PARSE", "1");
    else        unsetEnv("ADOCAO_FORCE_DOM_PARSE");
    LevelData lv;
    bool ok = lv.loadFromFile(file);
    unsetEnv("ADOCAO_FORCE_DOM_PARSE");
    return digest(lv, ok);
}

// 强制走 actions 分块并行解析。fixture 都小于自动阈值，不显式指定就永远跑不到
// 这条路径（parseActionRange 是独立实现，必须被用例覆盖）。
Digest loadForcedParallel(const std::string& file) {
    setEnv("ADOCAO_PARSE_PIECES", "4");
    LevelData lv;
    bool ok = lv.loadFromFile(file);
    unsetEnv("ADOCAO_PARSE_PIECES");
    return digest(lv, ok);
}

std::vector<std::string> collect(int argc, char** argv) {
    std::vector<std::string> files;
    if (argc <= 1) return files;
    for (int i = 1; i < argc; i++) {
        std::error_code ec;
        fs::path p = argv[i];
        if (fs::is_directory(p, ec)) {
            std::vector<std::string> found;
            for (auto& e : fs::directory_iterator(p, ec))
                if (e.is_regular_file() && e.path().extension() == ".json")
                    found.push_back(e.path().string());
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        } else {
            files.push_back(p.string());
        }
    }
    return files;
}

}  // namespace

int main(int argc, char** argv) {
    adofai::archive::install();   // 解压后端（core 只认接口）
    std::vector<std::string> files = collect(argc, argv);
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s <level file|fixture dir> ...\n", argv[0]);
        return 2;
    }

    if (const std::string e = levelPathResolveSelfTest(); !e.empty()) {
        std::printf("FAIL 路径解析自检: %s\n", e.c_str());
        return 1;
    }
    std::printf("ok   路径解析自检（文件夹→唯一谱；多个/不存在原样返回）\n");

    if (const std::string e = trackVisAllocationSelfTest(); !e.empty()) {
        std::printf("FAIL TrackVis 按需分配断言: %s\n", e.c_str());
        return 1;
    }
    std::printf("ok   TrackVis 按需分配（数组非空 ⟺ 消费方判据，三种谱 x 两条路径）\n");

    if (const std::string e = rawVolumeSemanticsSelfTest(); !e.empty()) {
        std::printf("FAIL 音量语义断言: %s\n", e.c_str());
        return 1;
    }
    std::printf("ok   音量语义断言（负/零音量原样保留，两条路径都查）\n");

    if (const std::string e = windowHugeValueSelfTest(); !e.empty()) {
        std::printf("FAIL 超大单值跨窗用例: %s\n", e.c_str());
        return 1;
    }
    std::printf("ok   超大单值跨窗用例（200 KB 字符串 + 500 个装饰物 / 4 KB 半窗）\n");

    if (const std::string e = windowBoundarySelfTest(); !e.empty()) {
        std::printf("FAIL 窗口边界用例（284 KB 合成谱 / 4 KB 半窗）: %s\n", e.c_str());
        return 1;
    }
    std::printf("ok   窗口边界用例（284 KB 合成谱 / 4 KB 半窗，2000 个跨窗 action）\n");

    if (const std::string e = streamSelfTest(); !e.empty()) {
        std::printf("FAIL 半窗流式解压自检: %s\n", e.c_str());
        return 1;
    }
    std::printf("ok   半窗流式解压自检（256 KB 伪数据 / 4 KB 半窗 / xz+zstd）\n");

    int failed = 0, rejected = 0;
    for (const std::string& f : files) {
        Digest legacy = loadPath(f, true);
        Digest fast   = loadPath(f, false);
        Digest again  = loadPath(f, false);

        const std::string a = diffSections(legacy, fast);
        const std::string b = diffSections(fast, again);
        const std::string c = fast.ok ? archiveRoundTrip(f, fast) : std::string();
        const std::string d = fast.ok ? diffSections(fast, loadForcedParallel(f)) : std::string();
        if (!a.empty() || !b.empty() || !c.empty() || !d.empty()) {
            failed++;
            std::printf("FAIL %s\n", f.c_str());
            if (!a.empty()) std::printf("     cleanJson+DOM vs 快路径: %s\n", a.c_str());
            if (!b.empty()) std::printf("     快路径两次加载不一致:   %s\n", b.c_str());
            if (!c.empty()) std::printf("     压缩容器往返:           %s\n", c.c_str());
            if (!d.empty()) std::printf("     顺序 vs 强制分块:       %s\n", d.c_str());
            std::printf("     angle=%zu/%zu actions=%zu/%zu tiles=%zu/%zu settings=%016llx/%016llx\n",
                        legacy.angleCount, fast.angleCount, legacy.actionCount, fast.actionCount,
                        legacy.tileCount, fast.tileCount,
                        (unsigned long long)legacy.settings, (unsigned long long)fast.settings);
        } else if (!fast.ok) {
            rejected++;   // 两条路都拒绝（真的不是 JSON）——一致即可
            std::printf("fail %s  (两条路都判为无法解析，一致)\n", f.c_str());
        } else {
            std::printf("ok   %s  angles=%zu actions=%zu tiles=%zu\n",
                        f.c_str(), fast.angleCount, fast.actionCount, fast.tileCount);
        }
    }

    std::printf("\n%d 个用例：%d 通过，%d 不一致，%d 双方都无法解析\n",
                (int)files.size(), (int)files.size() - failed - rejected, failed, rejected);
    return failed == 0 ? 0 : 1;
}
