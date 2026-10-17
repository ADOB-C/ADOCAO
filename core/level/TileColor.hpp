#pragma once
// 砖色模型：与 Re_ADOJAS `src/lib/Player/TileColorManager.ts` 逐条对齐。
//
// 分两层（对应它的 COLOR_FUNCS + FLOOR_FUNCS）：
//   1) 基色：由 `trackColorType`（Single/Stripes/Glow/Blink/Switch/Rainbow/Volume）
//      从 trackColor / secondaryTrackColor 和脉冲百分比算出一支"本体色"；
//   2) 样式：`trackStyle` 决定 fill/stroke 怎么从本体色派生：
//        Standard  fill=本体         stroke=0.7×本体
//        Neon      fill=#000000      stroke=本体
//        NeonLight fill=0.5×本体     stroke=本体
//        Basic     fill=本体         stroke=#000000
//        Gems      fill=本体         stroke=0.7×本体   （几何不同，颜色同 Standard）
//        Minimal   fill=本体         stroke=本体
//      `addBlack(0.7)` 是**逐通道 ×0.7**、`halfColor` 是 **÷2**（不是向黑混合 70%）。
//
// 渲染侧（docs/rendering.md）：
//   * Standard/Gems：顶点 mask 路 —— 内芯 fill、外圈 stroke；
//   * Neon/NeonLight/Basic/Minimal：2×128 渐变条路 —— base×strip(uv.y)，
//     uv.y 0=外缘、1=中心；base 由 style 选 fill 还是 stroke（Neon/NeonLight 用 stroke）。
// 本模型只给颜色；mask/条带的选择在渲染侧。
//
// 逐帧与静态：只有 Glow/Blink/Rainbow/Volume 是**时间驱动**（另外 TweenColor 期间
// Single/Stripes 也逐帧）。Single/Stripes 静态色可在加载时算一次。

#include <cstdint>
#include <string>
#include <string_view>

namespace adofai {

// 一个颜色段的参数 = 一条 ColorTrack 事件（或 settings 的默认值）。
// 字段名/缺省值与 Re_ADOJAS `createShiftType` 一致。
struct TileColorConfig {
    std::string colorType = "Single";     // trackColorType
    std::string color = "debb7b";         // trackColor
    std::string secondary = "ffffff";     // secondaryTrackColor
    std::string pulse = "None";           // trackColorPulse: None/Forward/Backward
    float  animDuration = 2.0f;           // trackColorAnimDuration（秒）
    float  pulseLength = 10.0f;           // trackPulseLength（层）
    std::string style = "Standard";       // trackStyle
    float  glowIntensity = 100.0f;        // trackGlowIntensity（只透传，不参与 fill/stroke）
    int    startFloor = 0;                // 事件所在层（Stripes 奇偶 / pulse 的基准层）
    double startTime = 0.0;               // 事件生效时刻（秒；settings 默认 = 0）
};

struct TileColors {
    std::uint8_t fill[3] = {0, 0, 0};
    std::uint8_t stroke[3] = {0, 0, 0};
    float opacity = 1.0f;                 // 8 位 hex 的 AA / 255，否则 1
};

// Glow/Blink/Rainbow/Volume：结果随时间变（必须逐帧重算）。
// 其余类型（Single/Stripes）只在 TweenColor 期间逐帧。
bool isTimeDrivenColorType(std::string_view type);

// 解析 "#RRGGBB[AA]" / "RRGGBB[AA]"；失败回退 0。
struct Rgb8 { std::uint8_t r = 0, g = 0, b = 0; std::uint8_t a = 255; };
Rgb8 parseColorHex(std::string_view hex);

// t/floor 时刻的 final fill/stroke。`amplitude` 只给 Volume 用（0..1）。
TileColors computeTileColors(const TileColorConfig& cfg, double timeSec, int floor,
                             float amplitude = 0.0f);

// 只做 FLOOR_FUNCS：给定本体色（已由颜色类型算出）返回 fill/stroke。
// 暴露出来便于单测与"静态色一次算好"的调用方复用。
void applyTrackStyle(std::string_view style, std::uint8_t baseR, std::uint8_t baseG,
                     std::uint8_t baseB, std::uint8_t outFill[3], std::uint8_t outStroke[3]);

}  // namespace adofai
