#include "app/HitsoundTypes.hpp"
#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>
#include <mutex>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

void drawHitsoundsPage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;

    drawConfigHeader(ch, "Hitsounds", 25.0f);

    const float colX = padX;
    ImGui::SetCursorPosX(colX);
    ImGui::Checkbox("Enable hitsounds", &st.enableHitsounds);
    if (!st.enableHitsounds) ImGui::BeginDisabled();
    ImGui::Spacing();
    ImGui::Indent(24.0f * S);
    ImGui::Checkbox("Override hit sound type", &st.forceHS);
    if (st.forceHS) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f * S);
        ImGui::Combo("##forceHSType", &st.forceHSIdx, kHitsoundTypes.data(), (int)kHitsoundTypes.size());
    }
    ImGui::Unindent(24.0f * S);
    if (!st.enableHitsounds) ImGui::EndDisabled();

    // ---- Preload（原先是单独一页 ✗，现在内联在这里）----
    // **控件与 Welcome 页一致 = 复选框**（用户明确要求：别做成蓝色按钮 ✗ —— 同一个词两种控件很乱）
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX);
    const bool wasOn = st.preloadEnabled;
    ImGui::Checkbox("Preload", &st.preloadEnabled);
    // 在这里**勾上**就立刻预载（Welcome 那边没勾、或上次失败时，这是一个补救入口 ✓）
    const bool running = st.preloadRunning && !st.preloadFinished.load();
    const bool done = st.cfg.preloadedLevel && st.cfg.preloadedTimeline;
    // 用户要求："加载就别显示出来" → 这一页**不显示任何加载指示**（进度条/状态文字都去掉 ✓），
    // 只保留复选框与失败时的红字。
    if (st.preloadEnabled && !wasOn && !running && !done) wizard::startPreload(st);
    if (!st.lastError.empty()) {
        ImGui::SetCursorPosX(colX);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", st.lastError.c_str());
    }

    bool back = false;
    if (drawNavButton(ch, false, true, /*showBack=*/true, &back) || back) {
        st.page = back ? Page::Welcome : Page::Graphics;   // Back → 回 Welcome（能改谱/重新预载）
    }
}

} // namespace wizard
