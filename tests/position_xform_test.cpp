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

    if (failures) {
        std::printf("position_xform: %d 处不一致\n", failures);
        return 1;
    }
    std::printf("position_xform: 全部通过（继承 / justThisTile / 通道独立 / 消极对照）\n");
    return 0;
}
