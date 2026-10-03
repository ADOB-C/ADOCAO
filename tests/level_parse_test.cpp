// 关卡解析对拍测试：快路径必须与 cleanJson + RapidJSON 老路径逐位一致。
//
// LevelData::tryFastParse() 绕过了 cleanJson()，直接在 mmap 出来的原文上扫描，所以它
// 必须逐条复现 cleanJson() 的容错规则。level_fixtures/ 里每个固定用例对应一条规则
// （生成脚本见 gen_level_fixtures.py）：
//   * 漏写逗号：值后面直接跟 " / { / [        （老编辑器/老版本会这么写）
//   * 前置逗号 / 重复逗号 / 尾随逗号           （丢掉）
//   * CRLF 行尾、字符串里的裸 CR              （丢掉）
//   * 数字/字符串/布尔/整数各变体、BOM、只有 pathData、空数组、最小文件 …
//
// 每个用例加载两遍：一遍强制走老路径（ADOCAO_FORCE_DOM_PARSE=1），一遍走快路径，
// 然后按节比较 LevelData（angleData / actions / settings / tiles / 每条派生数组），
// 逐位相同才算通过。快路径还会再加载一遍，确认自身可重复（并行/缓存不得引入抖动）。
//
// 用法：
//   adocao_level_parse_test <目录或文件> ...        目录会展开成其中的 *.json
// 也可以直接喂真实谱面，例如：
//   adocao_level_parse_test ~/Documents/Charts/**/*.adofai

#include "core/level/LevelData.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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
        ac.bytes(&x.flag, 1); ac.str(x.str);
    }
    H t;  for (auto& x : lv.tiles) {
        t.i32(x.index); t.f32(x.angle); t.f32(x.direction); t.bytes(&x.position, sizeof(x.position));
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
    H hv; hashMap(lv.tileHitsoundVolumes, hv);
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
    d.hsVolumes = hv(); d.hsVolumeCount = lv.tileHitsoundVolumes.size();
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

Digest loadPath(const std::string& file, bool legacy) {
    if (legacy) setEnv("ADOCAO_FORCE_DOM_PARSE", "1");
    else        unsetEnv("ADOCAO_FORCE_DOM_PARSE");
    LevelData lv;
    bool ok = lv.loadFromFile(file);
    unsetEnv("ADOCAO_FORCE_DOM_PARSE");
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
    std::vector<std::string> files = collect(argc, argv);
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s <level file|fixture dir> ...\n", argv[0]);
        return 2;
    }

    int failed = 0, rejected = 0;
    for (const std::string& f : files) {
        Digest legacy = loadPath(f, true);
        Digest fast   = loadPath(f, false);
        Digest again  = loadPath(f, false);

        const std::string a = diffSections(legacy, fast);
        const std::string b = diffSections(fast, again);
        if (!a.empty() || !b.empty()) {
            failed++;
            std::printf("FAIL %s\n", f.c_str());
            if (!a.empty()) std::printf("     cleanJson+DOM vs 快路径: %s\n", a.c_str());
            if (!b.empty()) std::printf("     快路径两次加载不一致:   %s\n", b.c_str());
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
