#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

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

    if (drawNavButton(ch, false, true)) {
        st.page = Page::Graphics;
    }
}

} // namespace wizard
