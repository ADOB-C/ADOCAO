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
    // Cull block granularity (coarse AABB index: cost/memory only, output is unaffected)
    ImGui::SetCursorPosX(colX); ImGui::Text("Cull block");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(contentW - (ctrlX - padX) - 24.0f * S);
    ImGui::Combo("##cullblock", &st.cullBlockIdx, kCullBlockNames.data(),
                 (int)kCullBlockNames.size());
    // 这里原来挂过一个悬停说明（"每块多少层 / 4k≈21MB / 256≈320MB / 不改变出图"）——
    // 用户指出"其他地方都没有"，向导里只有这一个控件有 tooltip，很突兀 → 去掉 ✗。
    // 这份权衡有正式去处：`docs/rendering.md`「可见性/剔除缓存」+ `tests/cull_cost_probe.cpp`。
    ImGui::Spacing();

    bool back = false;
    if (drawNavButton(ch, false, true, /*showBack=*/true, &back) || back) {
        st.page = back ? Page::Hitsounds : Page::Graphics;
    }
}

} // namespace wizard
