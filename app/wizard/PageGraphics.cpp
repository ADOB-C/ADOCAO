#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

void drawGraphicsPage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;
    const int winW = ch.winW;

    drawConfigHeader(ch, "Graphics", 50.0f);

    const float contentW = (float)winW - 2.0f * padX;
    const float colX = padX;
    const float ctrlX = padX + 150.0f * S;

    // Resolution
    ImGui::SetCursorPosX(colX); ImGui::Text("Resolution");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(contentW - (ctrlX - padX) - 24.0f * S);
    ImGui::Combo("##reso", &st.resoIdx, kResoNames.data(), (int)kResoNames.size());
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Checkbox("Fullscreen", &st.fullscreen);
    ImGui::Spacing();
    // MSAA (Off/2x/4x/8x)
    ImGui::SetCursorPosX(colX); ImGui::Text("MSAA");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(contentW - (ctrlX - padX) - 24.0f * S);
    ImGui::Combo("##msaa", &st.msaaIdx, kMsaaNames.data(), (int)kMsaaNames.size());
    ImGui::Spacing();
    // 剔除粗块粒度（只影响剔除成本与索引内存，不改出图）
    ImGui::SetCursorPosX(colX); ImGui::Text("剔除块粒度");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(contentW - (ctrlX - padX) - 24.0f * S);
    ImGui::Combo("##cullblock", &st.cullBlockIdx, kCullBlockNames,
                 (int)(sizeof(kCullBlockValues) / sizeof(kCullBlockValues[0])));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("更小 → 折叠/绕圈谱剔除更快，但索引内存按比例增大\n"
                          "0.0098 B/砖 @4096（2³¹−1 外推 21 MB）；256 层则 320 MB\n"
                          "只影响剔除成本与内存，**不改画面**");
    }
    ImGui::SetCursorPosX(colX);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted("不改画面；只影响剔除成本与索引内存");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (drawNavButton(ch, false, true)) {
        st.page = Page::Visuals;
    }
}

} // namespace wizard
