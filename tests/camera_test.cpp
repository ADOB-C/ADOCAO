// 两层摄像机（core/camera/CameraDirector）的**跨语言对拍**。
//
// 金标不是手算的：同一批事件分别喂给
//   * Re_ADOJAS 的 `src/lib/Player/CameraController.ts`（权威实现，node 直接跑）
//   * 我们的 `CameraDirector`（本文件）
// 逐帧比较 (x, y, rotation, zoom, followMode)。采样规则：dt = 1/60，每 15 帧一个点，
// 时间 t = k * (1.0/60.0)（两边同一算式，避免"采样点落在砖切换的浮点边界上"）。
//
// 8 个场景覆盖：Tile 位移、Kill(complete) 瞬跳、LastPosition 继承旋转、zoom + `[null,null]`
// dedup、无 position 的 Player 怪路径、legacyCamRelativeTo 累加、NaN 归零、OutBack 过冲。

#include "core/camera/CameraDirector.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#include "camera_gold.inc"

// 生成器写的是 `LevelData` 的位值；常量一改就必须重新生成金标。
static_assert((int)adofai::LevelData::kCamHasPosition == 1, "kCamHasPosition 位值变了");
static_assert((int)adofai::LevelData::kCamHasX == 2, "kCamHasX 位值变了");
static_assert((int)adofai::LevelData::kCamHasY == 4, "kCamHasY 位值变了");
static_assert((int)adofai::LevelData::kCamHasRotation == 8, "kCamHasRotation 位值变了");
static_assert((int)adofai::LevelData::kCamHasZoom == 16, "kCamHasZoom 位值变了");
static_assert((int)adofai::LevelData::kCamInactive == 32, "kCamInactive 位值变了");
static_assert((int)adofai::LevelData::kCamEditorOnly == 64, "kCamEditorOnly 位值变了");
static_assert((int)adofai::LevelData::kCamEaseWritten == 128, "kCamEaseWritten 位值变了");

namespace {

constexpr float  kBpm = 120.0f;
constexpr double kSpb = 60.0 / 120.0;
constexpr int    kNFramesSample = 15;
constexpr int    kNTiles = 40;

int g_fail = 0;

int tileAt(double t) {
    const int i = (int)std::floor(t / kSpb);
    return i < 0 ? 0 : (i >= kNTiles ? kNTiles - 1 : i);
}

void runScenario(const GoldScenario& sc) {
    adofai::LevelData::Settings s;
    s.position = {sc.posX, sc.posY};
    s.rotation = sc.rot;
    s.zoom = sc.zoom;
    s.relativeTo = sc.relTo;
    s.legacyCamRelativeTo = sc.legacy;

    std::vector<adofai::CameraKey> keys;
    keys.reserve((size_t)sc.keyCount);
    for (int i = 0; i < sc.keyCount; i++) {
        const GoldKey& g = sc.keys[i];
        adofai::CameraKey k;
        k.time = g.time;
        k.anchorX = g.ax;
        k.anchorY = g.ay;
        k.bpm = g.bpm;
        k.ev.floor = g.floor;
        k.ev.x = g.x;
        k.ev.y = g.y;
        k.ev.rotation = g.rot;
        k.ev.zoom = g.zoom;
        k.ev.duration = g.dur;
        k.ev.ease = g.ease;
        k.ev.relativeTo = g.relTo;
        k.ev.flags = g.flags;
        k.ev.disabled = 0;
        k.ev.id = 0xFFFFFFFFu;
        k.ev.order = (uint32_t)i;
        keys.push_back(k);
    }

    adofai::CameraDirector dir;
    dir.reset(s, keys, 0.0, 0.0, kBpm);

    const double dt = 1.0 / 60.0;
    int sampleIdx = 0;
    int failuresBefore = g_fail;
    for (int step = 0; step < sc.sampleCount * kNFramesSample; step++) {
        const double t = (double)step * dt;
        const double px = (double)tileAt(t);
        dir.update(t, dt, kBpm, 1.0, px, 0.0, tileAt(t));
        if (step % kNFramesSample != 0) continue;
        const GoldSample& g = sc.samples[sampleIdx++];
        if (g.k != step) {
            std::fprintf(stderr, "FAIL %s: 采样下标错位（%d != %d）\n", sc.name, step, g.k);
            ++g_fail;
            continue;
        }
        const double dx = std::fabs(dir.x() - g.x);
        const double dy = std::fabs(dir.y() - g.y);
        const double dr = std::fabs(dir.rotation() - g.rot);
        const double dz = std::fabs(dir.zoom() - g.zoom);
        const int follow = dir.followMode() ? 1 : 0;
        // 容差 1e-9：两边都是 double，公式逐行相同；差异只可能来自 libm 的 ulp
        // （OutBack 里的 pow 等），1e-9 远大于它、又远小于肉眼可见的量。
        if (dx > 1e-9 || dy > 1e-9 || dr > 1e-9 || dz > 1e-9 || follow != g.follow) {
            if (++g_fail <= 20) {
                std::fprintf(stderr,
                             "FAIL %s @k=%d: pos=(%.12g,%.12g) want=(%.12g,%.12g) rot=%.12g/%.12g "
                             "zoom=%.12g/%.12g follow=%d/%d\n",
                             sc.name, step, dir.x(), dir.y(), g.x, g.y,
                             dir.rotation(), g.rot, dir.zoom(), g.zoom, follow, g.follow);
            }
        }
    }
    if (g_fail == failuresBefore) std::printf("ok   %s（%d 个采样点）\n", sc.name, sc.sampleCount);
}

}  // namespace

int main() {
    const int n = (int)(sizeof(kGoldScenarios) / sizeof(kGoldScenarios[0]));
    for (int i = 0; i < n; i++) runScenario(kGoldScenarios[i]);

    if (g_fail) { std::fprintf(stderr, "camera: %d 处不一致\n", g_fail); return 1; }
    std::printf("camera: %d 个场景逐帧对上 Re_ADOJAS 的 CameraController（容差 1e-9）\n", n);
    return 0;
}
