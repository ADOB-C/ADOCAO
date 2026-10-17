#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

// Colors 页：轨道填充 / 描边 / 背景（从原 Visuals 页拆出来的后半段，见 PageTrail.cpp 顶部说明）。
void drawColorsPage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;

    drawConfigHeader(ch, "Colors", 80.0f);

    const float colX = padX;
    const float ctrlX = padX + 150.0f * S;

    // Fill / stroke / background colors. ImGui's color widget: swatch opens the
    // built-in picker, the inline field stays hex (matches --fill/--stroke/--bg).
    ImGui::SetCursorPosX(colX); ImGui::Text("Fill");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(160.0f * S);
    ImGui::ColorEdit3("##fill", st.fillColor.data(), ImGuiColorEditFlags_DisplayHex);
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Checkbox("Auto stroke", &st.autoStroke);
    ImGui::Spacing();
    ImGui::Indent(24.0f * S);
    if (st.autoStroke)
        st.strokeColor = deriveStroke(st.fillColor.data());   // fill * 0.5
    if (st.autoStroke) ImGui::BeginDisabled();
    ImGui::SetCursorPosX(colX + 24.0f * S); ImGui::Text("Stroke");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(160.0f * S);
    ImGui::ColorEdit3("##stroke", st.strokeColor.data(), ImGuiColorEditFlags_DisplayHex);
    if (st.autoStroke) ImGui::EndDisabled();
    ImGui::Unindent(24.0f * S);
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Text("Background");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(160.0f * S);
    ImGui::ColorEdit3("##bg", st.bgColor.data(), ImGuiColorEditFlags_DisplayHex);
    ImGui::Spacing();

    bool back = false;
    // Back 时 drawNavButton 返回 false（只有 `back` 出参知道按下去了）→ 必须 `|| back`
    if (drawNavButton(ch, false, true, /*showBack=*/true, &back) || back) {
        st.page = back ? prevPage(st.page) : nextPage(st.page);   // Trail → Colors → Music
    }
}

} // namespace wizard
