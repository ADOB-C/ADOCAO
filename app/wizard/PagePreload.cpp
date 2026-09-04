#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>
#include <mutex>

namespace wizard {

// Transient page shown while the background preload (parse + timeline) runs.
// The wizard loop watches State::preloadFinished and advances to Hitsounds
// (or back to Welcome on failure) on its own.
void drawPreloadPage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;
    const int winW = ch.winW;
    const float contentW = (float)winW - 2.0f * padX;

    ImGui::Spacing();
    ImGui::Spacing();
    float barW = contentW;
    ImGui::SetCursorPosX(padX);
    if (ch.mainFont) ImGui::PushFont(ch.mainFont);
    ImVec2 t0 = ImGui::CalcTextSize("Preloading level...");
    ImGui::SetCursorPosX(((float)winW - t0.x) / 2.0f);
    ImGui::Text("Preloading level...");
    ImGui::Spacing();
    ImGui::SetCursorPosX(padX);
    float pct = st.preloadProgress.percent.load();
    ImGui::ProgressBar(pct / 100.0f, ImVec2(barW, 22.0f * S));
    ImGui::Spacing();
    {
        std::lock_guard<std::mutex> lock(st.preloadProgress.textMutex);
        ImVec2 ts2 = ImGui::CalcTextSize(st.preloadProgress.stageText);
        ImGui::SetCursorPosX(((float)winW - ts2.x) / 2.0f);
        ImGui::Text("%s", st.preloadProgress.stageText);
    }
    if (ch.mainFont) ImGui::PopFont();
}

} // namespace wizard
