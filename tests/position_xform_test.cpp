// PositionTrack 的 rotation / scale / opacity（2026-11-02）
//
// 权威：`Re_ADOJAS/src/lib/Player/PositionTrackManager.ts`（b4590e4）
//   `tileScale = s; if (!justThisTile) curScale = s;`（rotation/opacity 同构），
//   然后 `workScale[floor] = tileScale !== null ? tileScale : curScale`；
//   消费侧 `Player.ts:6855` 把它当作 mesh 的**基础**变换（rotation.z / scale / material.opacity）。
// 即：**事件值总写本砖**，持久状态只在**非** justThisTile 时推进；没写就继承。
//
// 用法：`ctest --test-dir build`（或直接跑 build/tests/adocao_position_xform_test）

#include "core/level/LevelData.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;

static int failures = 0;

static void expectXf(const LevelData& lv, int64_t floor, float rot, float scale, float op,
                     const char* why) {
    const LevelData::PosTrackXform x = lv.posTrackXformAt(floor);
    const bool ok = std::fabs(x.rotDeg - rot) < 1e-4f && std::fabs(x.scale - scale) < 1e-4f &&
                    std::fabs(x.opacity - op) < 1e-4f;
    if (ok) return;
    std::printf("  ✗ floor %lld: 期望 rot=%.3f scale=%.3f op=%.3f，实际 %.3f/%.3f/%.3f —— %s\n",
                (long long)floor, rot, scale, op, x.rotDeg, x.scale, x.opacity, why);
    failures++;
}

int main() {
    // 继承型（Disabled）：floor 2 → scale 90%，floor 4 → rotation 30° / opacity 50%
    static const char* kLevel = R"JSON({"settings": {"bpm": 60},
 "angleData": [0, 0, 0, 0, 0, 0, 0, 0],
 "actions": [
   {"floor": 2, "eventType": "PositionTrack", "scale": 90},
   {"floor": 4, "eventType": "PositionTrack", "rotation": 30, "opacity": 50},
   {"floor": 6, "eventType": "PositionTrack", "scale": 200, "justThisTile": "Enabled"}
 ]})JSON";

    LevelData lv;
    if (!lv.loadFromBuffer(kLevel, std::strlen(kLevel))) {
        std::printf("✗ 用例谱面没加载成功\n");
        return 1;
    }
    if (!lv.hasPosTrackXform()) {
        std::printf("✗ hasPosTrackXform() 应为 true（谱里写了 scale/rotation/opacity）\n");
        return 1;
    }

    // 事件之前：恒等
    expectXf(lv, 0, 0.0f, 1.0f, 1.0f, "事件之前是恒等");
    expectXf(lv, 1, 0.0f, 1.0f, 1.0f, "事件之前是恒等（紧邻）");

    // floor 2 的 scale 90% → 本层生效并**继承**到之后所有层，直到被改写
    expectXf(lv, 2, 0.0f, 0.9f, 1.0f, "事件值写本砖");
    expectXf(lv, 3, 0.0f, 0.9f, 1.0f, "非 justThisTile → 继承到下一层");

    // floor 4 再写 rotation/opacity：**只改写了的通道**，scale 继续继承 0.9
    expectXf(lv, 4, 30.0f, 0.9f, 0.5f, "未写的通道保持继承值");
    expectXf(lv, 5, 30.0f, 0.9f, 0.5f, "继续继承");

    // floor 6 是 justThisTile：本砖 200%，**下一层回到 90%**（核心断言）
    expectXf(lv, 6, 30.0f, 2.0f, 0.5f, "justThisTile 只改本砖");
    expectXf(lv, 7, 30.0f, 0.9f, 0.5f, "justThisTile **不许**推进持久状态");

    // 消极对照：没写的谱不该分配任何 run/override
    static const char* kPlain = R"JSON({"settings": {"bpm": 60}, "angleData": [0, 0, 0],
 "actions": [{"floor": 1, "eventType": "Twirl"}]})JSON";
    {
        LevelData lv2;
        if (!lv2.loadFromBuffer(kPlain, std::strlen(kPlain))) {
            std::printf("  ✗ 对照谱面没加载成功\n");
            failures++;
        } else if (lv2.hasPosTrackXform() || lv2.posTrackRuns().size() != 0) {
            std::printf("  ✗ 对照谱不该有 PositionTrack 变换（runs=%zu）\n",
                        lv2.posTrackRuns().size());
            failures++;
        }
    }

    // ── relativeTo（2026-11-02）──────────────────────────────────────────────
    // 权威 `IDFromTile`：ThisTile → floor+off；Start → off；End → n-1+off，都 clamp 到 [0,n-1]。
    // 算法：change = (baseX[target]+bitSum(target)+point(target)) − (baseX[floor]+vector)。
    // angleData 全 0 → 基础位置就是 (i, 0)，一眼可读。
    static const char* kRel = R"JSON({"settings": {"bpm": 60},
 "angleData": [0, 0, 0, 0, 0, 0, 0, 0],
 "actions": [
   {"floor": 3, "eventType": "PositionTrack", "relativeTo": [2, "ThisTile"],  "justThisTile": "Enabled"},
   {"floor": 4, "eventType": "PositionTrack", "relativeTo": [-4, "Start"],    "justThisTile": "Enabled"},
   {"floor": 6, "eventType": "PositionTrack", "relativeTo": [0, "End"],       "justThisTile": "Disabled"}
 ]})JSON";
    {
        LevelData lv3;
        if (!lv3.loadFromBuffer(kRel, std::strlen(kRel))) {
            std::printf("  ✗ relativeTo 用例没加载成功\n");
            failures++;
        } else {
            auto pos = [&](int f) { return lv3.tiles[(size_t)f].position[0]; };
            // floor 3 → target 5：change = 5 − 3 = +2，点加只影响本层
            if (std::fabs(pos(3) - 5.0) > 1e-9) {
                std::printf("  ✗ relativeTo ThisTile: floor3 期望 x=5，实际 %.3f\n", pos(3)); failures++;
            }
            if (std::fabs(pos(4) - 4.0 - (-4.0)) > 1e-9) {
                std::printf("  ✗ relativeTo Start: floor4 期望 x=0，实际 %.3f\n", pos(4)); failures++;
            }
            // floor 6 → target 8（End，n−1）：change = 8 − 6 = +2，继承型 → 6..8 都 +2
            if (std::fabs(pos(6) - 8.0) > 1e-9 || std::fabs(pos(7) - 9.0) > 1e-9 ||
                std::fabs(pos(8) - 10.0) > 1e-9) {
                const auto& e6 = lv3.tilePositionOffsets.at(6)[0];
                std::printf("  ✗ relativeTo End: 期望 8/9/10，实际 %.3f/%.3f/%.3f"
                            "（mask=%u relType=%u relOff=%d n=%zu）\n",
                            pos(6), pos(7), pos(8), (unsigned)e6.posMask,
                            (unsigned)e6.relType, (int)e6.relOffset, lv3.tiles.size()); failures++;
            }
            if (std::fabs(pos(5) - 5.0) > 1e-9) {     // 5 不在 floor6 的继承范围之前
                std::printf("  ✗ relativeTo 不该动 floor5，实际 %.3f\n", pos(5)); failures++;
            }
        }
    }

    if (failures) {
        std::printf("position_xform: %d 处不一致\n", failures);
        return 1;
    }
    std::printf("position_xform: 全部通过（继承 / justThisTile / 通道独立 / 消极对照）\n");
    return 0;
}
