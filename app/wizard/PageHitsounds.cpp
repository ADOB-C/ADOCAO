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
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX);
    if (ImGui::Button("Preload", ImVec2(110.0f * S, 30.0f * S))) {
        wizard::startPreload(st);        // 再预载一次（会先取消并汇合已在跑的那个）
    }
    ImGui::SameLine();
    if (st.preloadRunning && !st.preloadFinished.load()) {
        ImGui::TextUnformatted("Preloading level...");
    } else if (st.cfg.preloadedLevel && st.cfg.preloadedTimeline) {
        ImGui::TextUnformatted("Preloaded - start will be fast.");
    } else {
        ImGui::TextUnformatted("Not preloaded (loads on Start).");
    }
    if (st.preloadRunning && !st.preloadFinished.load()) {
        float pct = st.preloadProgress.percent.load();
        ImGui::SetCursorPosX(colX);
        ImGui::ProgressBar(pct / 100.0f, ImVec2(ImGui::GetContentRegionAvail().x - 24.0f * S, 20.0f * S));
        std::lock_guard<std::mutex> lock(st.preloadProgress.textMutex);
        ImGui::SetCursorPosX(colX);
        ImGui::TextDisabled("%s", st.preloadProgress.stageText);
    }
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
