#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

// Colors 页：轨道填充 / 描边 / 背景（从原 Visuals 页拆出来的后半段，见 PageTrail.cpp 顶部说明）。
// 版面走 WizardChrome 的表单栅格：Fill / Background 是顶格字段行，Stroke 从属于 Auto stroke（缩进 24）。
void drawColorsPage(State& st, const Chrome& ch) {
    formStyleBegin();
    drawConfigHeader(ch, "Colors", 80.0f);

    // ImGui 的颜色控件：色块打开取色器，内联字段仍是十六进制（与 --fill/--stroke/--bg 同一口径）
    formLabel(ch, "Fill");
    ImGui::SetNextItemWidth(formCtrlW(ch));
    ImGui::ColorEdit3("##fill", st.fillColor.data(), ImGuiColorEditFlags_DisplayHex);

    formCheckbox(ch, "Auto stroke", &st.autoStroke);
    if (st.autoStroke)
        st.strokeColor = deriveStroke(st.fillColor.data());   // fill * 0.5
    if (st.autoStroke) ImGui::BeginDisabled();
    formLabel(ch, "Stroke", /*sub=*/true);
    ImGui::SetNextItemWidth(formCtrlW(ch, /*sub=*/true));
    ImGui::ColorEdit3("##stroke", st.strokeColor.data(), ImGuiColorEditFlags_DisplayHex);
    if (st.autoStroke) ImGui::EndDisabled();

    formLabel(ch, "Background");
    ImGui::SetNextItemWidth(formCtrlW(ch));
    ImGui::ColorEdit3("##bg", st.bgColor.data(), ImGuiColorEditFlags_DisplayHex);

    formStyleEnd();

    bool back = false;
    // Back 时 drawNavButton 返回 false（只有 `back` 出参知道按下去了）→ 必须 `|| back`
    if (drawNavButton(ch, false, true, /*showBack=*/true, &back) || back) {
        st.page = back ? prevPage(st.page) : nextPage(st.page);   // Trail → Colors → Music
    }
}

} // namespace wizard
