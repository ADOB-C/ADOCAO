#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

namespace wizard {

void drawVisualsPage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;

    drawConfigHeader(ch, "Visuals", 75.0f);

    const float colX = padX;
    const float ctrlX = padX + 150.0f * S;

    // Trail
    ImGui::SetCursorPosX(colX);
    ImGui::Checkbox("Trail", &st.showTrail);
    if (!st.showTrail) ImGui::BeginDisabled();
    ImGui::Spacing();

    // Length: seconds (default) or tiles (optional)
    ImGui::SetCursorPosX(colX); ImGui::Text("Duration");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##dur", &st.trailDuration, 0.005f, 0.05f, 2.0f, "%.2fs");
    if (st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Text("Length in tiles");
    ImGui::SameLine(ctrlX);
    ImGui::Checkbox("##ltiles", &st.trailLengthInTiles);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Measure the trail in tiles instead of seconds. A time window\n"
                          "is unusable on charts that speed up by orders of magnitude:\n"
                          "0.4s is ~6800 tiles at BPM 1.5M, so the trail sweeps across\n"
                          "the map and lands in front of the planet. In this mode the\n"
                          "sample rate follows the track automatically.");
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Text("Trail tiles");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (!st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##tiles", &st.trailTiles, 0.25f, 1.0f, 256.0f, "%.2f");
    if (!st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();

    // Rate: fixed Hz by default, optionally raised with the track speed.
    // The tiles mode above derives its own rate, so these two are moot there.
    ImGui::SetCursorPosX(colX); ImGui::Text("Sample rate");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##srate", &st.trailSampleRate, 2.0f, 5.0f, 500.0f, "%.0f/s");
    if (st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Text("Speed-aware sampling");
    ImGui::SameLine(ctrlX);
    if (st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::Checkbox("##pertile", &st.trailPerTile);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Raise the sample rate to follow the track covered per second\n"
                          "(step + the arc of the tile's relative angle). Fixes charts\n"
                          "that speed up by orders of magnitude, and slow tiles that\n"
                          "sweep a large angle. Never lower than the rate above.");
    if (st.trailLengthInTiles) ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Text("Samples / tile");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(140.0f * S);
    if (!st.trailPerTile && !st.trailLengthInTiles) ImGui::BeginDisabled();
    ImGui::DragFloat("##spt", &st.trailSamplesPerTile, 0.25f, 0.5f, 32.0f, "%.2f");
    if (!st.trailPerTile && !st.trailLengthInTiles) ImGui::EndDisabled();

    if (!st.showTrail) ImGui::EndDisabled();
    ImGui::Spacing();

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

    if (drawNavButton(ch, false, true)) {
        st.page = Page::Music;
    }
}

} // namespace wizard
