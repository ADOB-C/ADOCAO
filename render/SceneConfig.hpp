#pragma once
// `LevelScene` 的构建期配置。**故意不依赖 app 的 LauncherConfig**：ADOCAO 的窗口层和
// ADOFAO-E 这类下游都要能构造它（下游曾经因为拿不到 `LevelScene` 而整份复制过 577 行）。
#include <functional>
#include <string>

namespace adofai {

struct SceneConfig {
    // 逐砖颜色（ColorTrack / trackStyle）默认开启；只有显式 --fill/--stroke 才整体覆盖。
    bool trackColorOverride = false;
    std::string trackFillColor = "DEBB7B";
    std::string trackStrokeColor = "6F5D3D";
    bool autoStroke = true;          // 描边 = 填充 × 0.5

    bool showTrail = true;
    float trailDuration = 0.4f;
    float trailSampleRate = 800.0f;
    bool trailAdaptive = true;
    float trailTargetFps = 120.0f;
    float trailRateMin = 60.0f;
    float trailRateMax = 4000.0f;
    bool trailPerTile = false;
    float trailSamplesPerTile = 4.0f;
    bool trailLengthInTiles = false;
    float trailTiles = 8.0f;
    float trailSagitta = 0.0f;       // >0 = 按"每样本偏离 ≤ N px"反推采样率

    int cullBlockFloors = 4096;      // 剔除块粒度（0 = 全扫）
    bool deterministicCull = false;  // 抓帧模式：关掉所有由帧时间反馈决定的量
    bool showAllTiles = false;       // 忽略 appear/disappear（编辑器全图视角）

    // **宿主注入**：当前常驻内存字节数（app 用 `appmem::currentResidentBytes()`）。
    // 留空 = 不打这条日志。库里（render/ 会被镜像）不能 include `app/**`，
    // 所以这个量具只能由宿主给 —— 早先直接 include `app/ProcMem.hpp`，下游镜像直接断。
    std::function<size_t()> residentBytes;
};

}  // namespace adofai
