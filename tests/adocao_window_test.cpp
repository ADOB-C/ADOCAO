// ③「每砖结构窗口化」自测：**从一个 checkpoint + 一块数据重算出来的每砖数据，必须与全量加载
// 逐位相同**（memcmp，不是容差）。这是 ③ 唯一的验收手段 —— 位置/BPM/时间都是串行前缀和，
// 从中间续算只要起点状态或浮点类型差一点就会漂，而"漂一点"在渲染上是完全看不出来的。
//
// 三个必须覆盖的边界（都对不上就说明 §8.8 的三条不变量没落地）：
//   ① `floor == i0` 的事件：继承型要算进本层、点加型只作用于本层、Twirl/SetSpeed 要影响本层；
//   ② 中旋 999 跨窗口边界（`floats[i] = floats[i-1] + 180` 要能从快照推回上一层的有效方向）；
//   ③ SetSpeed 的 **float** 累乘（生产路径是 float，用 double 会在多次乘之后差 ULP）。

#include "archive/AdocaoCheckpoints.hpp"
#include "archive/AdocaoWindow.hpp"
#include "archive/AdocaoReader.hpp"
#include "archive/AdocaoWriter.hpp"
#include "archive/Install.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;
using namespace adofai::adocao;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) std::printf("ok   %s\n", what.c_str());
    else { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// 把一张谱**逐窗**重算，并与全量加载的 13 个节里跟窗口有关的那几条逐位比。
// 返回不一致的层数（0 = 全同）；firstBad 给出首个不一致的层号。
struct WindowDiff {
    size_t tiles = 0, dirs = 0, bpms = 0, times = 0;
    uint64_t firstBad = 0;
    uint64_t windows = 0;
};

WindowDiff verifyAllWindows(const LevelData& lv) {
    WindowDiff d;
    Timeline tl;
    tl.build(lv, /*exportOnly=*/true);
    const std::vector<double>& fullTimes = tl.tileStartTimes();
    const std::vector<float>& fullBPMs = tl.tileBPMs();

    std::vector<FloorCheckpoint> ck;
    std::string err;
    if (!computeCheckpoints(lv, kCheckpointStride, ck, err)) {
        std::printf("FAIL computeCheckpoints: %s\n", err.c_str());
        ++g_fail;
        return d;
    }

    const size_t nData = lv.angleData.size();
    for (const FloorCheckpoint& c : ck) {
        const uint64_t i0 = c.floor;
        const size_t count = std::min<size_t>(kCheckpointStride, nData - (size_t)i0);
        if (count == 0) break;
        FloorWindowInput in;
        in.checkpoint = c;
        std::vector<double> angles(lv.angleData.begin() + (size_t)i0,
                                   lv.angleData.begin() + (size_t)i0 + count);
        std::vector<LevelData::FastAction> events;
        for (const LevelData::FastAction& a : lv.actions)
            if (a.floor >= (int)i0 && (uint64_t)a.floor < i0 + count) events.push_back(a);
        in.angles = &angles;
        in.events = &events;

        FloorWindow w;
        if (!computeFloorWindow(lv.settings, in, w, err)) {
            std::printf("FAIL computeFloorWindow@%llu: %s\n", (unsigned long long)i0, err.c_str());
            ++g_fail;
            return d;
        }
        ++d.windows;
        for (size_t k = 0; k < count; ++k) {
            const size_t i = (size_t)i0 + k;
            bool bad = false;
            if (std::memcmp(&w.tiles[k].position[0], &lv.tiles[i].position[0], 8) != 0 ||
                std::memcmp(&w.tiles[k].position[1], &lv.tiles[i].position[1], 8) != 0) {
                ++d.tiles; bad = true;
            }
            if (std::memcmp(&w.tiles[k].direction, &lv.tiles[i].direction, 4) != 0) {
                ++d.dirs; bad = true;
            }
            if (i < fullBPMs.size() && std::memcmp(&w.tileBPMs[k], &fullBPMs[i], 4) != 0) {
                ++d.bpms; bad = true;
            }
            if (i < fullTimes.size() && std::memcmp(&w.tileStartTimes[k], &fullTimes[i], 8) != 0) {
                ++d.times; bad = true;
            }
            if (bad && d.firstBad == 0) d.firstBad = i;
        }
    }
    return d;
}

LevelData loadText(const std::string& text) {
    LevelData lv;
    if (!lv.loadFromString(text)) std::printf("(合成谱加载失败)\n");
    return lv;
}

std::string anglesJson(int n, int period, double step, bool midspins) {
    std::string t = "{\"angleData\":[";
    for (int i = 0; i < n; ++i) {
        if (i) t += ',';
        if (midspins && (i % period == period - 1)) t += "999";
        else t += std::to_string((int)((i * step)) % 360);
    }
    t += "],\"settings\":{\"bpm\":150.0},\"actions\":[";
    return t;
}

// ---- ① 纯角度：位置/方向/时间三样都要逐位同 ---------------------------------
void plainAngles() {
    LevelData lv = loadText(anglesJson(9000, 0, 37.0, false) + "]}");
    const WindowDiff d = verifyAllWindows(lv);
    check(d.tiles == 0 && d.dirs == 0 && d.bpms == 0 && d.times == 0,
          fmt("纯角度 9000 层 / %llu 窗：位置/方向/BPM/时间全部逐位相同（不一致 %zu/%zu/%zu/%zu @%llu）",
              (unsigned long long)d.windows, d.tiles, d.dirs, d.bpms, d.times,
              (unsigned long long)d.firstBad));
}

// ---- ② 块边界上的事件（继承 / 点加 / Twirl / SetSpeed 都落在 4096 上）--------
void eventsOnBoundary() {
    std::string t = anglesJson(9000, 0, 37.0, false);
    t += "{\"floor\":4096,\"eventType\":\"PositionTrack\",\"positionOffset\":[2.5,-1.5],\"justThisTile\":\"Disabled\"},";
    t += "{\"floor\":4097,\"eventType\":\"PositionTrack\",\"positionOffset\":[0.5,0.25],\"justThisTile\":\"Enabled\"},";
    t += "{\"floor\":8192,\"eventType\":\"Twirl\"},";
    t += "{\"floor\":8192,\"eventType\":\"SetSpeed\",\"speedMultiplier\":1.25,\"beatsPerMinute\":0},";
    t += "{\"floor\":8193,\"eventType\":\"SetSpeed\",\"speedMultiplier\":0,\"beatsPerMinute\":222.5},";
    t += "{\"floor\":4096,\"eventType\":\"Pause\",\"duration\":3,\"angleOffset\":0}";
    t += "]}";
    LevelData lv = loadText(t);
    const WindowDiff d = verifyAllWindows(lv);
    check(d.tiles == 0 && d.dirs == 0 && d.bpms == 0 && d.times == 0,
          fmt("块边界事件（继承/点加/Twirl/SetSpeed×2/Pause 全落在 4096 上）：逐位相同"
              "（不一致 %zu/%zu/%zu/%zu @%llu）",
              d.tiles, d.dirs, d.bpms, d.times, (unsigned long long)d.firstBad));
}

// ---- ③ 中旋 999 跨窗口边界 ---------------------------------------------------
void midspinAcrossBoundary() {
    // 让 4095/4096/4097 连续三层都是 999 —— 窗口起点正好落在 999 连跑中间
    std::string t = "{\"angleData\":[";
    for (int i = 0; i < 9000; ++i) {
        if (i) t += ',';
        const bool sentinel = (i >= 4095 && i <= 4097) || (i >= 8190 && i <= 8194);
        t += sentinel ? "999" : std::to_string((int)((i * 53) % 360));
    }
    t += "],\"settings\":{\"bpm\":150.0},\"actions\":[]}";
    LevelData lv = loadText(t);
    const WindowDiff d = verifyAllWindows(lv);
    check(d.tiles == 0 && d.dirs == 0 && d.bpms == 0 && d.times == 0,
          fmt("中旋 999 跨窗口边界（4095..4097 / 8190..8194 连跑）：逐位相同"
              "（不一致 %zu/%zu/%zu/%zu @%llu）",
              d.tiles, d.dirs, d.bpms, d.times, (unsigned long long)d.firstBad));
}

// ---- ④ 反面对照：动 checkpoint 的一位，必须变红 ------------------------------
void sensitivity() {
    LevelData lv = loadText(anglesJson(9000, 0, 37.0, false) + "]}");
    std::vector<FloorCheckpoint> ck;
    std::string err;
    computeCheckpoints(lv, kCheckpointStride, ck, err);
    if (ck.size() < 2) { check(false, "反面对照：前置条件不成立"); return; }

    const uint64_t i0 = ck[1].floor;
    const size_t count = kCheckpointStride;
    std::vector<double> angles(lv.angleData.begin() + (size_t)i0,
                               lv.angleData.begin() + (size_t)i0 + count);
    std::vector<LevelData::FastAction> events;
    FloorWindowInput in;
    in.checkpoint = ck[1];
    in.angles = &angles;
    in.events = &events;
    FloorWindow w;
    check(computeFloorWindow(lv.settings, in, w, err), "反面对照：前置条件（正常窗口）可算");
    check(std::memcmp(&w.tiles[0].position[0], &lv.tiles[i0].position[0], 8) == 0,
          "反面对照：正常窗口首层位置逐位相同");

    // (a) baseX 动 1 ULP
    {
        FloorWindowInput bad = in;
        bad.checkpoint.baseX = std::nextafter(bad.checkpoint.baseX, bad.checkpoint.baseX + 1.0);
        FloorWindow wb;
        computeFloorWindow(lv.settings, bad, wb, err);
        check(std::memcmp(&wb.tiles[0].position[0], &lv.tiles[i0].position[0], 8) != 0,
              "反面对照：baseX 动 1 ULP → 首层位置必须变红");
    }
    // (b) bpm 变一点。注意口径：checkpoint 里的 bpm 是**生产路径的 float 值**（存进 double），
    // 所以动 1 个 double-ULP 是**看不见的**（nextafter 之后转回 float 还是同一个数）——
    // 这不是缺陷而是"必须用 float"的直接体现（见 AdocaoWindow.hpp 的浮点类型警告）。
    // 所以这里动一个 float 能分辨的量。
    {
        FloorWindowInput bad = in;
        bad.checkpoint.bpm = (double)((float)bad.checkpoint.bpm * 1.5f);
        FloorWindow wb;
        computeFloorWindow(lv.settings, bad, wb, err);
        Timeline tl;
        tl.build(lv, true);
        bool timeDiff = false;
        for (size_t k = 0; k < count; ++k)
            if (std::memcmp(&wb.tileStartTimes[k], &tl.tileStartTimes()[i0 + k], 8) != 0) {
                timeDiff = true;
                break;
            }
        check(wb.tileBPMs[0] != w.tileBPMs[0] && timeDiff,
              "反面对照：bpm 变 1.5× → tileBPMs 与 tileStartTimes 都必须变红");
    }
    // (c) cw 翻一位（Twirl 的初值）
    {
        FloorWindowInput bad = in;
        bad.checkpoint.cw = bad.checkpoint.cw ? 0 : 1;
        FloorWindow wb;
        computeFloorWindow(lv.settings, bad, wb, err);
        Timeline tl;
        tl.build(lv, true);
        bool diff = false;
        for (size_t k = 0; k < count; ++k)
            if (std::memcmp(&wb.tileStartTimes[k], &tl.tileStartTimes()[i0 + k], 8) != 0) { diff = true; break; }
        check(diff, "反面对照：cw 翻一位 → tileStartTimes 必须变红");
    }
    // (d) 缺分量：flags 抹掉 time
    {
        FloorWindowInput bad = in;
        bad.checkpoint.flags &= ~kCkTime;
        FloorWindow wb;
        check(!computeFloorWindow(lv.settings, bad, wb, err),
              "反面对照：checkpoint 缺 time 分量 → 明确失败（不许拿 0 当时间）");
    }
}

// ---- ⑤ 容器里的按层索引：区间 == 全量扫描 -----------------------------------
void actionIndex(const LevelData& lv, const char* what) {
    std::vector<uint8_t> bytes;
    std::string err;
    PackResult rep;
    PackOptions opts;
    opts.withCheckpoints = true;
    if (!packLevel(lv, bytes, err, &rep, opts)) {
        check(false, fmt("%s：pack 失败 %s", what, err.c_str()));
        return;
    }
    LevelData raw;
    DerivedInfo info;
    std::string e2;
    if (!unpackLevel(bytes.data(), bytes.size(), raw, e2, &info)) {
        check(false, fmt("%s：unpackLevel 失败 %s", what, e2.c_str()));
        return;
    }
    if (info.firstAction.size() < 2) {
        check(false, fmt("%s：没有 ActionIndex 段（actions 可能不是按 floor 有序）", what));
        return;
    }
    const size_t nData = lv.angleData.size();
    const uint64_t nBlocks = (nData + info.actionStride - 1) / info.actionStride;
    check(info.firstAction.size() == nBlocks + 1,
          fmt("%s：按层索引有 %zu 条 = 块数 %llu + 1（哨兵）",
              what, info.firstAction.size(), (unsigned long long)nBlocks));

    // 逐块与"全量扫描"对照
    size_t bad = 0;
    uint64_t firstBadBlock = 0;
    for (uint64_t b = 0; b < nBlocks; ++b) {
        const uint64_t lo = b * info.actionStride;
        const uint64_t hi = std::min<uint64_t>(lo + info.actionStride, nData);
        size_t wantLo = 0, wantHi = 0;
        bool seenLo = false;
        for (size_t i = 0; i < lv.actions.size(); ++i) {
            const int f = lv.actions[i].floor;
            if (f < 0) continue;
            if (!seenLo && (uint64_t)f >= lo) { wantLo = i; seenLo = true; }
            if ((uint64_t)f < hi) wantHi = i + 1;
        }
        if (!seenLo) wantLo = lv.actions.size();
        wantHi = std::max(wantHi, wantLo);
        if (info.firstAction[b] != wantLo || info.firstAction[b + 1] != wantHi) {
            if (bad == 0) firstBadBlock = b;
            ++bad;
        }
    }
    check(bad == 0, fmt("%s：%llu 个块的 action 区间全部与全量扫描一致（不一致 %zu%s）",
                        what, (unsigned long long)nBlocks, bad,
                        bad ? fmt("，首个 @块 %llu", (unsigned long long)firstBadBlock).c_str() : ""));
}

}  // namespace

int main(int argc, char** argv) {
    adofai::archive::install();
    std::printf("— ③ 每砖结构窗口化自测 —\n");
    plainAngles();
    eventsOnBoundary();
    midspinAcrossBoundary();
    sensitivity();

    // 真谱：窗口重算 + 按层索引都在真实规模上过一遍
    for (int i = 1; i < argc; ++i) {
        LevelData lv;
        if (!lv.loadFromFile(argv[i])) { std::printf("加载失败: %s\n", argv[i]); return 1; }
        const WindowDiff d = verifyAllWindows(lv);
        std::printf("     真谱 %s：%zu 层 / %llu 窗｜位置 %zu、方向 %zu、BPM %zu、时间 %zu 不一致%s\n",
                    argv[i], lv.angleData.size(), (unsigned long long)d.windows,
                    d.tiles, d.dirs, d.bpms, d.times,
                    (d.tiles + d.dirs + d.bpms + d.times) ? fmt("（首个 @%llu）",
                        (unsigned long long)d.firstBad).c_str() : " ✓");
        if (d.tiles + d.dirs + d.bpms + d.times) ++g_fail;
        actionIndex(lv, argv[i]);
    }

    if (g_fail) { std::printf("\n%d 项失败\n", g_fail); return 1; }
    std::printf("\n全部通过\n");
    return 0;
}
