#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

// Trail 页：拖尾本身（开关 + 时长/长度/采样率）。原来和颜色挤在同一个 Visuals 页里，
// 一页 11 行，最后一行离底部按钮只剩几像素（改字号时实测被 Back/Next 盖住 ✗）
// —— 2026-10-16 拆成 Trail / Colors 两页，页序 Graphics → Trail → Colors → Music。
// 版面走 WizardChrome 的表单栅格：组头是左列复选框，6 行参数整块缩进 24（`sub = true`）。
void drawTrailPage(State& st, const Chrome& ch) {
    formStyleBegin();
    drawConfigHeader(ch, "Trail", 60.0f);

    formCheckbox(ch, "Trail", &st.showTrail);
    if (!st.showTrail) ImGui::BeginDisabled();

    // Length: seconds (default) or tiles (optional)
    formLabel(ch, "Duration", /*sub=*/true);
    ImGui::SetNextItemWidth(formCtrlW(ch, /*sub=*/true));
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##dur", &st.trailDuration, 0.005f, 0.05f, 2.0f, "%.2fs");
    if (st.trailLengthInTiles) ImGui::EndDisabled();

    formLabel(ch, "Length in tiles", /*sub=*/true);
    ImGui::Checkbox("##ltiles", &st.trailLengthInTiles);

    formLabel(ch, "Trail tiles", /*sub=*/true);
    ImGui::SetNextItemWidth(formCtrlW(ch, /*sub=*/true));
    if (!st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##tiles", &st.trailTiles, 0.25f, 1.0f, 256.0f, "%.2f");
    if (!st.trailLengthInTiles) ImGui::EndDisabled();

    // Rate: fixed Hz by default, optionally raised with the track speed.
    // The tiles mode above derives its own rate, so these two are moot there.
    formLabel(ch, "Sample rate", /*sub=*/true);
    ImGui::SetNextItemWidth(formCtrlW(ch, /*sub=*/true));
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##srate", &st.trailSampleRate, 2.0f, 5.0f, 500.0f, "%.0f/s");
    if (st.trailLengthInTiles) ImGui::EndDisabled();

    formLabel(ch, "Speed-aware sampling", /*sub=*/true);
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::Checkbox("##pertile", &st.trailPerTile);
    if (st.trailLengthInTiles) ImGui::EndDisabled();

    formLabel(ch, "Samples / tile", /*sub=*/true);
    ImGui::SetNextItemWidth(formCtrlW(ch, /*sub=*/true));
    if (!st.trailPerTile && !st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##spt", &st.trailSamplesPerTile, 0.25f, 0.5f, 32.0f, "%.2f");
    if (!st.trailPerTile && !st.trailLengthInTiles) ImGui::EndDisabled();

    if (!st.showTrail) ImGui::EndDisabled();

    formStyleEnd();

    bool back = false;
    // Back 时 drawNavButton 返回 false（只有 `back` 出参知道按下去了）→ 必须 `|| back`
    if (drawNavButton(ch, false, true, /*showBack=*/true, &back) || back) {
        st.page = back ? prevPage(st.page) : nextPage(st.page);   // Graphics → Trail → Colors
    }
}

} // namespace wizard
