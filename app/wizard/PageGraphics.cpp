#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

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

    if (drawNavButton(ch, false, true)) {
        st.page = Page::Visuals;
    }
}

} // namespace wizard
