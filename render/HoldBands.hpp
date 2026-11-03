// Hold 长按带（DLC 长按事件的可视化，2026-11-03）
//
// 权威 `Re_ADOJAS/src/lib/Player/HoldRenderer.ts` + `Player.ts::buildHoldRenderers`：
//   带子 = 长按期间行星的**真实轨迹** —— 半径 r（tileSize × radiusScale）恒定绕一个**平移的圆心**：
//     起点圆心 = Hold 砖位置；终点圆心 = 下一砖位置 − rTail·dir(exitAngle)，
//     其中 exitAngle = startAngle + totalAngle（**基础**出口角，不能用 atan2(下一砖−本砖)）；
//     角度从 startAngle 扫完 totalAngle（含 holdLength 整圈），半径 rStart→rTail 线性。
//   宽度 = max(0.15, 0.275 × (rStart + rTail) / 2)；UV.y 沿路径（进度），UV.x 横跨带宽。
//   每块 Hold 砖建**一次** mesh，进度只改 uniform（不做官方式全量重画）。
//
// 与参考的差异（记在 docs/rendering.md）：彩虹贴图没有，用"球色 + 中央白线"近似。
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace adofai {

class LevelData;
class Timeline;
class Shader;
class Camera;

class HoldBands {
public:
    void build(const LevelData& lv, const Timeline& tl);
    void destroy();
    bool empty() const { return m_bands.empty(); }
    void draw(Shader& shader, const Camera& camera, double camX, double camY, double timeSec,
              const LevelData& lv, const class TileAnimation* anim);

private:
    struct Band {
        unsigned vao = 0, vbo = 0, ebo = 0;
        int indexCount = 0;
        float probe[15] = {0};   // 前 3 个顶点（诊断用）
        glm::dvec2 center{0.0, 0.0};   // 相机相对偏移用（取起点圆心）
        int64_t floor = 0;
        double startTime = 0.0, endTime = 0.0;
    };
    std::vector<Band> m_bands;
    unsigned m_tex = 0;   // 程序生成的彩虹带贴图（一次）
};

}  // namespace adofai
