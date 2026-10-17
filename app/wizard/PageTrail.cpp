#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

// Trail 页：拖尾本身（开关 + 时长/长度/采样率）。原来和颜色挤在同一个 Visuals 页里，
// 一页 11 行，最长的那行离底部按钮只剩几像素（改字号时实测被 Back/Next 盖住 ✗）
// —— 2026-10-16 按用户要求拆成 Trail / Colors 两页，页序 Graphics → Trail → Colors → Music。
void drawTrailPage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;

    drawConfigHeader(ch, "Trail", 60.0f);

    const float colX = padX;
    const float ctrlX = padX + 150.0f * S;
    // 复选框下的 6 行**整块缩进 24px**（标签与控件一起右移，保持控件列对齐）：
    // 这是向导里既有的"子选项"写法 —— Hitsounds 页的 "Override hit sound type"、
    // Colors 页的 "Stroke" 都是 24px（用户："我记得之前控件有缩进啊" ✓）。
    const float rowX = colX + 24.0f * S;
    const float ctrlRowX = ctrlX + 24.0f * S;

    // Trail
    ImGui::SetCursorPosX(colX);
    ImGui::Checkbox("Trail", &st.showTrail);
    if (!st.showTrail) ImGui::BeginDisabled();
    ImGui::Spacing();

    // Length: seconds (default) or tiles (optional)
    ImGui::SetCursorPosX(rowX); ImGui::Text("Duration");
    ImGui::SameLine(ctrlRowX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##dur", &st.trailDuration, 0.005f, 0.05f, 2.0f, "%.2fs");
    if (st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX(rowX); ImGui::Text("Length in tiles");
    ImGui::SameLine(ctrlRowX);
    ImGui::Checkbox("##ltiles", &st.trailLengthInTiles);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Measure the trail in tiles instead of seconds. A time window\n"
                          "is unusable on charts that speed up by orders of magnitude:\n"
                          "0.4s is ~6800 tiles at BPM 1.5M, so the trail sweeps across\n"
                          "the map and lands in front of the planet. In this mode the\n"
                          "sample rate follows the track automatically.");
    ImGui::Spacing();
    ImGui::SetCursorPosX(rowX); ImGui::Text("Trail tiles");
    ImGui::SameLine(ctrlRowX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (!st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##tiles", &st.trailTiles, 0.25f, 1.0f, 256.0f, "%.2f");
    if (!st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();

    // Rate: fixed Hz by default, optionally raised with the track speed.
    // The tiles mode above derives its own rate, so these two are moot there.
    ImGui::SetCursorPosX(rowX); ImGui::Text("Sample rate");
    ImGui::SameLine(ctrlRowX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##srate", &st.trailSampleRate, 2.0f, 5.0f, 500.0f, "%.0f/s");
    if (st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX(rowX); ImGui::Text("Speed-aware sampling");
    ImGui::SameLine(ctrlRowX);
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::Checkbox("##pertile", &st.trailPerTile);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Raise the sample rate to follow the track covered per second\n"
                          "(step + the arc of the tile's relative angle). Fixes charts\n"
                          "that speed up by orders of magnitude, and slow tiles that\n"
                          "sweep a large angle. Never lower than the rate above.");
    if (st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX(rowX); ImGui::Text("Samples / tile");
    ImGui::SameLine(ctrlRowX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (!st.trailPerTile && !st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##spt", &st.trailSamplesPerTile, 0.25f, 0.5f, 32.0f, "%.2f");
    if (!st.trailPerTile && !st.trailLengthInTiles) ImGui::EndDisabled();

    if (!st.showTrail) ImGui::EndDisabled();
    ImGui::Spacing();

    bool back = false;
    // Back 时 drawNavButton 返回 false（只有 `back` 出参知道按下去了）→ 必须 `|| back`
    if (drawNavButton(ch, false, true, /*showBack=*/true, &back) || back) {
        st.page = back ? prevPage(st.page) : nextPage(st.page);   // Graphics → Trail → Colors
    }
}

} // namespace wizard
