#pragma once

#include <cstdint>
#include <vector>

#include "core/level/LevelData.hpp"

namespace adofai {

// 一条**已展开**的运镜事件：时间、宿主砖的锚点与 BPM、以及原样的字段。
// 时间与 `Timeline::tileStartTimes` 同一基准（关卡时间线，秒）。
// 展开规则照抄 Re_ADOJAS 的 TimelineManager（docs/camera.md §2.2/§2.3）：
//   * 事件时间 = tileStartTimes[floor] + (angleOffset/180) * (60/bpm[floor])；
//   * RepeatEvents 的 Beat 模式：留在基砖，angleOffset += interval * rep * 180；
//   * Floor 模式：`executeOnCurrentFloor` 时搬到目标砖、angleOffset 用事件自己的；
//     否则留在基砖，angleOffset 用**几何拍差**（entryBeat 之差 × 180，与变速无关）；
//   * 同刻（|Δt| < 0.1 ms）按 `id` 升序，再按文件顺序 —— 与 Re_ADOJAS 的排序键一致。
struct CameraKey {
    double time = 0.0;
    double anchorX = 0.0;     // 事件宿主砖的世界坐标（Tile 模式的锚点；构建期解析成 double）
    double anchorY = 0.0;
    float  bpm = 100.0f;      // 宿主砖 BPM（事件时长用，不是跟随速度）
    LevelData::CameraEvent ev;
};

class CameraTimeline {
public:
    // 构建失败只有一种情况：level 没有相机事件（空时间线是合法结果，不是错误）。
    // tileStartTimes / tileBPMs 来自 Timeline（含末尾 extra 砖，与 Re_ADOJAS 一致）。
    bool empty() const { return m_keys.empty(); }
    void build(const LevelData& level,
               const std::vector<double>& tileStartTimes,
               const std::vector<float>& tileBPMs);

    const std::vector<CameraKey>& keys() const { return m_keys; }

    // 诊断计数（测试/日志用）：被过滤掉的事件数（inactive / editorOnly）与展开出的条目数。
    size_t droppedInactive() const { return m_droppedInactive; }

private:
    std::vector<CameraKey> m_keys;
    size_t m_droppedInactive = 0;
};

}  // namespace adofai
