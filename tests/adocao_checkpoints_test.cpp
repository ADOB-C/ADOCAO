// `.adocao` v3 `Derived` 段（§8.8 ⑧）自测：**checkpoint 的唯一验收手段**是
// "从快照重算 [i0,i1) 的 position，与全谱一次算的 `tiles[i].position` **逐位** memcmp"
// （§8.10⑥）。所以这条用例的重点不是"能编能解"，而是：
//
//   ① 快照语义 = "`floor < i0` 的事件全部处理完"（闭包语义，§8.8 不变量 1）；
//   ② 窗口重算**从快照重新生成**，绝不原地累加（不变量 2）；
//   ③ 窗口的事件扫描**包含 `floor == i0`**（`justThisTile` 是点加，不变量 3）；
//   ④ **反面对照**：把快照的 baseX 动 1 个 ULP，对拍必须**变红** —— 否则"永远绿"的对拍等于没验。

#include "archive/AdocaoCheckpoints.hpp"
#include "archive/Install.hpp"
#include "archive/AdocaoReader.hpp"
#include "archive/AdocaoWriter.hpp"
#include "core/level/LevelData.hpp"

#include <cmath>
#include <cstdio>
#include <cstdarg>
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

// 合成一张谱并**走完整的加载收尾**（calculateTilePositions + processActions +
// applyPositionTrackOffsets）。为什么不直接摆 LevelData 的字段：那两个函数是私有的，
// 而"checkpoint 能不能续算"恰恰要对**生产路径算出来的** tiles 逐位负责。
LevelData makeLevel(const std::vector<double>& angles,
                    const std::vector<std::pair<int, LevelData::TilePositionOffset>>& offs,
                    float bpm = 120.0f) {
    std::string text = "{\"angleData\":[";
    for (size_t i = 0; i < angles.size(); ++i) {
        if (i) text += ',';
        text += std::to_string(angles[i]);
    }
    text += "],\"settings\":{\"bpm\":" + std::to_string(bpm) + "},\"actions\":[";
    for (size_t k = 0; k < offs.size(); ++k) {
        if (k) text += ',';
        text += "{\"floor\":" + std::to_string(offs[k].first) +
                ",\"eventType\":\"PositionTrack\",\"positionOffset\":[" +
                std::to_string(offs[k].second.offsetX) + "," +
                std::to_string(offs[k].second.offsetY) + "],\"justThisTile\":\"" +
                (offs[k].second.justThisTile ? "Enabled" : "Disabled") + "\"}";
    }
    text += "]}";
    LevelData lv;
    if (!lv.loadFromString(text)) std::printf("(makeLevel 加载失败)\n");
    return lv;
}

// 对每个 checkpoint 都从快照重算一整个 stride，逐位对拍
size_t verifyAll(const LevelData& lv, const std::vector<FloorCheckpoint>& ck, size_t* badAt = nullptr) {
    size_t bad = 0;
    for (const FloorCheckpoint& c : ck) {
        size_t first = 0;
        const size_t i0 = (size_t)c.floor;
        const size_t i1 = i0 + kCheckpointStride;
        const size_t n = verifyCheckpointResume(lv, c, i0, i1, &first);
        if (n != 0) {
            bad += n;
            if (badAt && bad == n) *badAt = first;
        }
    }
    return bad;
}

// ---- ① 无事件：纯 angleData 前缀和 ------------------------------------------
void plainPrefix() {
    std::vector<double> angles;
    for (int i = 0; i < 10000; ++i) angles.push_back(std::fmod(i * 137.508, 360.0));
    LevelData lv = makeLevel(angles, {});
    std::vector<FloorCheckpoint> ck;
    std::string err;
    check(computeCheckpoints(lv, kCheckpointStride, ck, err), "纯 angleData：computeCheckpoints 成功");
    check(ck.size() == 3, fmt("纯 angleData：10000 层 → %zu 条（0/4096/8192）", ck.size()));
    check(verifyAll(lv, ck) == 0, "纯 angleData：每个快照重算一整个 stride，position **逐位**相同");
}

// ---- ② PositionTrack：继承（后缀加）+ 点加（justThisTile）---------------------
void positionTrack() {
    std::vector<double> angles(9000, 90.0);
    std::vector<std::pair<int, LevelData::TilePositionOffset>> offs;
    // 继承型：floor 100 起 +[3,0]（本层及之后都吃到）
    offs.push_back({100, {3.0f, 0.0f, false}});
    // 点加型：floor 4096（**正好在块边界上**）只作用于本层 —— 不变量 3 的靶子
    offs.push_back({4096, {0.0f, 7.0f, true}});
    // ⚠️ `tilePositionOffsets` 是 **unordered_map<int,…>：一层只有一条**（同层多事件会互相覆盖，
    // 这是产品现状，不是本模块的事）。所以这里把"同层多事件"拆到相邻层，免得测的是覆盖行为。
    offs.push_back({4097, {0.25f, 0.5f, true}});      // 点加（justThisTile）
    offs.push_back({4098, {1.5f, -2.5f, false}});     // 继承
    offs.push_back({8192, {-4.0f, 4.0f, false}});
    LevelData lv = makeLevel(angles, offs);

    std::vector<FloorCheckpoint> ck;
    std::string err;
    check(computeCheckpoints(lv, kCheckpointStride, ck, err), "PositionTrack：computeCheckpoints 成功");
    check(ck.size() == 3, "PositionTrack：9000 层 → 3 条");
    // floor 0 的快照里 cum 必须还是 0（事件都在 floor >= 100）
    check(ck[0].cumX == 0.0 && ck[0].cumY == 0.0, "PositionTrack：floor 0 的快照 cum = 0");
    // floor 4096 的快照必须**已经含** floor 100 的继承偏移，但**不含** floor 4096 的点加
    check(ck[1].cumX == 3.0 && ck[1].cumY == 0.0,
          "PositionTrack：floor 4096 的快照含 floor 100 的继承量；点加不进 cum（不变量 1）");
    // floor 8192 的快照 = **严格 floor < 8192** 的继承量（floor 100 与 4098 两笔）；
    // 8192 自己那一笔属于"窗口内要扫到的事件"（不变量 3），**不许**预先算进快照
    check(ck[2].cumX == 3.0 + 1.5 && ck[2].cumY == 0.0 - 2.5,
          "PositionTrack：floor 8192 的快照只含 floor < 8192 的继承量（本层事件留给窗口）");
    size_t badAt = 0;
    const size_t bad = verifyAll(lv, ck, &badAt);
    check(bad == 0, fmt("PositionTrack：三个窗口逐位对拍全同（含 justThisTile 落在块边界那一层）bad=%zu@%zu",
                        bad, badAt));
}

// ---- ③ 中旋哨兵（angleData == 999）：方向按"上一层 +180"续 ----------------
void midspinSentinel() {
    std::vector<double> angles;
    for (int i = 0; i < 9000; ++i) {
        if (i % 7 == 3) angles.push_back(999.0);
        else angles.push_back(std::fmod(i * 91.7, 360.0));
    }
    LevelData lv = makeLevel(angles, {});
    std::vector<FloorCheckpoint> ck;
    std::string err;
    check(computeCheckpoints(lv, kCheckpointStride, ck, err), "中旋：computeCheckpoints 成功");
    // lastAngleDeg 必须是"最后一个**真**角度"，且标出上一层是不是哨兵
    bool okLast = true;
    const std::vector<double>& ad = lv.angleData;      // 走完加载收尾的才是权威值
    for (size_t k = 0; k < ck.size(); ++k) {
        const size_t i0 = k * kCheckpointStride;
        if (i0 == 0) continue;
        if ((ck[k].flags & kCkLastAngle) == 0) { okLast = false; break; }
        const double want = ad[i0 - 1];
        if (want == 999.0) {
            if ((ck[k].flags & kCkLastSentinel) == 0) { okLast = false; break; }
            size_t j = i0 - 1;                          // 真值 = i0 之前最后一个非 999
            while (j > 0 && ad[j] == 999.0) --j;
            if (ck[k].lastAngleDeg != ad[j]) { okLast = false; break; }
        } else if (ck[k].lastAngleDeg != want) { okLast = false; break; }
    }
    check(okLast, "中旋：lastAngleDeg = 最后一个真角度，999 用 kCkLastSentinel 标出");
    check(verifyAll(lv, ck) == 0, "中旋：逐位对拍全同（999 → 上一层 +180 的续算）");
}

// ---- ④ 反面对照：动 1 个 ULP，对拍必须变红 ---------------------------------
void sensitivity() {
    std::vector<double> angles(6000, 45.0);
    LevelData lv = makeLevel(angles, {});
    std::vector<FloorCheckpoint> ck;
    std::string err;
    if (!computeCheckpoints(lv, kCheckpointStride, ck, err) || ck.size() < 2) {
        check(false, "反面对照：前置条件不成立");
        return;
    }
    check(verifyAll(lv, ck) == 0, "反面对照：未改动时对拍全同（前置条件）");

    FloorCheckpoint bad = ck[1];
    bad.baseX = std::nextafter(bad.baseX, bad.baseX + 1.0);   // 1 ULP
    size_t first = 0;
    const size_t n = verifyCheckpointResume(lv, bad, (size_t)bad.floor,
                                            (size_t)bad.floor + kCheckpointStride, &first);
    check(n > 0 && first == (size_t)bad.floor,
          fmt("反面对照：baseX 动 1 ULP → 对拍必须红（实测 %zu/%zu 层不一致，首个 @%zu）",
              n, (size_t)kCheckpointStride, first));
}

// ---- ⑤ 编解码往返（f64 位模式原样）+ 坏数据干净失败 -------------------------
void codecRoundTrip() {
    std::vector<double> angles(9000, 137.5);
    LevelData lv = makeLevel(angles, {});
    std::vector<FloorCheckpoint> ck;
    std::string err;
    computeCheckpoints(lv, kCheckpointStride, ck, err);

    std::vector<uint8_t> blob;
    uint32_t stride = 0;
    check(encodeCheckpoints(ck, kCheckpointStride, blob), "编解码：encodeCheckpoints 成功");
    check(blob.size() == 16 + ck.size() * kCheckpointBytes,
          fmt("编解码：载荷 = 16 + %zu × %zu = %zu B", ck.size(), kCheckpointBytes, blob.size()));
    std::vector<FloorCheckpoint> back;
    check(decodeCheckpoints(blob.data(), blob.size(), stride, back) && stride == kCheckpointStride &&
          back.size() == ck.size() &&
          std::memcmp(back.data(), ck.data(), ck.size() * sizeof(FloorCheckpoint)) == 0,
          "编解码：逐字节往返（memcmp 整块结构）");

    std::vector<FloorCheckpoint> sink;
    check(!decodeCheckpoints(blob.data(), blob.size() / 2, stride, sink), "坏数据：截断必须被拒");
    check(!decodeCheckpoints(blob.data(), 8, stride, sink), "坏数据：连头都不完整必须被拒");
    {   // 层号不是 stride 的倍数
        std::vector<uint8_t> b = blob;
        uint64_t f = 1;
        std::memcpy(b.data() + 16, &f, 8);
        check(!decodeCheckpoints(b.data(), b.size(), stride, sink), "坏数据：floor 不是 stride 的倍数必须被拒");
    }
    {   // 层号不递增
        std::vector<uint8_t> b = blob;
        uint64_t f = 0;
        std::memcpy(b.data() + 16 + kCheckpointBytes, &f, 8);
        check(!decodeCheckpoints(b.data(), b.size(), stride, sink), "坏数据：floor 不递增必须被拒");
    }
    {   // stride = 0
        std::vector<uint8_t> b = blob;
        uint32_t z = 0;
        std::memcpy(b.data(), &z, 4);
        check(!decodeCheckpoints(b.data(), b.size(), stride, sink), "坏数据：stride=0 必须被拒");
    }
}

// ---- ⑥ 端到端：pack --checkpoints → unpack → 用**加载后的** LevelData 对拍 ----
void endToEnd() {
    // 合成一张谱：4096 边界上有 PositionTrack（点加 + 继承），还有中旋
    std::string text = "{\"angleData\":[";
    for (int i = 0; i < 9000; ++i) {
        if (i) text += ',';
        text += (i % 11 == 5) ? "999" : std::to_string((int)((i * 37) % 360));
    }
    text += "],\"settings\":{\"bpm\":180.0},\"actions\":[";
    text += "{\"floor\":100,\"eventType\":\"PositionTrack\",\"positionOffset\":[3,0],\"justThisTile\":\"Disabled\"},";
    text += "{\"floor\":4096,\"eventType\":\"PositionTrack\",\"positionOffset\":[0,7],\"justThisTile\":\"Enabled\"},";
    text += "{\"floor\":8192,\"eventType\":\"PositionTrack\",\"positionOffset\":[-2,1],\"justThisTile\":\"Disabled\"}";
    text += "]}";

    LevelData src;
    if (!src.loadFromString(text)) { check(false, "端到端：合成谱加载失败"); return; }
    std::vector<uint8_t> bytes;
    std::string err;
    PackResult rep;
    PackOptions opts;
    opts.withCheckpoints = true;
    check(packLevel(src, bytes, err, &rep, opts) && rep.checkpoints == 3,
          fmt("端到端：pack 写出 %llu 条 checkpoint（%llu B）",
              (unsigned long long)rep.checkpoints, (unsigned long long)rep.checkpointBytes));

    // tiles 必须来自**走完收尾**的那条路（`loadFromBuffer` 会做 finishLoad）；
    // `unpackLevel` 刻意不做收尾（它与 JSON 路径共用同一段收尾），所以只用它取 Derived 段。
    LevelData lv;
    check(lv.loadFromBuffer((const char*)bytes.data(), bytes.size()),
          "端到端：.adocao 走完整加载（含收尾）");
    LevelData lvRaw;
    DerivedInfo info;
    std::string err2;
    check(unpackLevel(bytes.data(), bytes.size(), lvRaw, err2, &info) && info.checkpoints.size() == 3,
          "端到端：unpackLevel 把 Derived 段带出来了");
    check(info.stride == kCheckpointStride, "端到端：stride = 4096");
    size_t bad = 0;
    size_t badFloor = 0;
    for (const FloorCheckpoint& c : info.checkpoints) {
        size_t first = 0;
        const size_t n = verifyCheckpointResume(lv, c, (size_t)c.floor,
                                                (size_t)c.floor + kCheckpointStride, &first);
        if (n && bad == 0) { badFloor = first; }
        bad += n;
    }
    check(bad == 0, fmt("端到端：从 .adocao 读回来的 checkpoint 对拍**逐位**相同（bad=%zu@%zu）",
                        bad, badFloor));

    // 不写 checkpoint 时 Derived 段必须不存在（默认口径 = 与 v2 同大小）
    PackResult rep2;
    std::vector<uint8_t> bytes2;
    std::string err3;
    check(packLevel(src, bytes2, err3, &rep2) && rep2.checkpoints == 0 && bytes2.size() <= bytes.size(),
          "端到端：默认不写 Derived 段（文件更小或相同）");
    DerivedInfo info2;
    std::string err4;
    LevelData lv2;
    check(unpackLevel(bytes2.data(), bytes2.size(), lv2, err4, &info2) && info2.checkpoints.empty(),
          "端到端：没有 Derived 段时不算错误，checkpoints 为空");
}

// ---- 真谱模式：<谱> 走完整加载 → pack(--checkpoints) → 逐条快照对拍 -------------
// 为什么要单独留一个入口：4096 边界 + PositionTrack + 中旋要**几万层**才压得到，
// fixture 太小；而这条验收必须在真实规模的谱上也跑过（D5：命令 + 数字留痕）。
int realChart(const char* path) {
    LevelData lv;
    if (!lv.loadFromFile(path)) { std::printf("加载失败: %s\n", path); return 1; }
    std::vector<uint8_t> bytes;
    std::string err;
    PackResult rep;
    PackOptions opts;
    opts.withCheckpoints = true;
    if (!packLevel(lv, bytes, err, &rep, opts)) { std::printf("pack 失败: %s\n", err.c_str()); return 1; }
    LevelData loaded;
    if (!loaded.loadFromBuffer((const char*)bytes.data(), bytes.size())) {
        std::printf("回读失败（.adocao 加载）\n");
        return 1;
    }
    LevelData raw;
    DerivedInfo info;
    std::string err2;
    if (!unpackLevel(bytes.data(), bytes.size(), raw, err2, &info)) {
        std::printf("unpackLevel 失败: %s\n", err2.c_str());
        return 1;
    }
    size_t bad = 0, first = 0, floors = 0;
    for (const FloorCheckpoint& c : info.checkpoints) {
        size_t f = 0;
        const size_t n = verifyCheckpointResume(loaded, c, (size_t)c.floor,
                                                (size_t)c.floor + kCheckpointStride, &f);
        if (n && bad == 0) first = f;
        bad += n;
        floors += kCheckpointStride;
    }
    std::printf("真谱 %s：%zu 层 → %zu 条 checkpoint（%llu B / 容器 %zu B）｜"
                "从快照重算 %zu 层，不一致 %zu 层%s\n",
                path, loaded.angleData.size(), info.checkpoints.size(),
                (unsigned long long)rep.checkpointBytes, bytes.size(), floors, bad,
                bad ? fmt("（首个 @%zu）", first).c_str() : " ✓");
    return bad == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    adofai::archive::install();      // .adocao 的解码后端（core 只认接口，要显式注册）
    std::printf("— .adocao v3 Derived/checkpoint 自测（§8.8 ⑧）—\n");
    plainPrefix();
    positionTrack();
    midspinSentinel();
    sensitivity();
    codecRoundTrip();
    endToEnd();
    if (g_fail) { std::printf("\n%d 项失败\n", g_fail); return 1; }
    std::printf("\n单元用例全部通过\n");
    for (int i = 1; i < argc; ++i) {
        if (realChart(argv[i]) != 0) return 1;
    }
    return 0;
}
