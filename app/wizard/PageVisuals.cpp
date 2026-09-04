#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>
#include <cstdio>

namespace wizard {

namespace {

bool isValidHexColor(const char* buf) {
    int len = 0;
    while (buf[len]) len++;
    if (len != 6) return false;
    for (int i = 0; i < 6; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

} // namespace

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
    ImGui::Indent(24.0f * S);
    ImGui::SetCursorPosX(colX + 24.0f * S); ImGui::Text("Sample rate");
    ImGui::SameLine(ctrlX + 0.0f);
    ImGui::SetNextItemWidth(140.0f * S);
    ImGui::DragFloat("##srate", &st.trailSampleRate, 2.0f, 5.0f, 500.0f, "%.0f/s");
    ImGui::Unindent(24.0f * S);
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Text("Duration");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(140.0f * S);
    ImGui::DragFloat("##dur", &st.trailDuration, 0.005f, 0.05f, 2.0f, "%.2fs");
    if (!st.showTrail) ImGui::EndDisabled();
    ImGui::Spacing();

    // Fill / stroke / background colors
    bool fillOk   = isValidHexColor(st.fillBuf);
    bool strokeOk = isValidHexColor(st.strokeBuf);
    bool bgOk     = isValidHexColor(st.bgBuf);
    ImGui::SetCursorPosX(colX); ImGui::Text("Fill #");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(90.0f * S);
    ImGui::InputText("##fill", st.fillBuf, 7, ImGuiInputTextFlags_CharsUppercase);
    if (!fillOk && st.fillBuf[0]) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),"(6 hex)"); }
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Checkbox("Auto stroke", &st.autoStroke);
    ImGui::Spacing();
    ImGui::Indent(24.0f * S);
    if (st.autoStroke) ImGui::BeginDisabled();
    ImGui::SetCursorPosX(colX + 24.0f * S); ImGui::Text("Stroke #");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(90.0f * S);
    ImGui::InputText("##stroke", st.strokeBuf, 7, ImGuiInputTextFlags_CharsUppercase);
    if (st.autoStroke) ImGui::EndDisabled();
    if (!strokeOk && st.strokeBuf[0]) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),"(6 hex)"); }
    ImGui::Unindent(24.0f * S);
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX); ImGui::Text("Background #");
    ImGui::SameLine(ctrlX);
    ImGui::SetNextItemWidth(90.0f * S);
    ImGui::InputText("##bg", st.bgBuf, 7, ImGuiInputTextFlags_CharsUppercase);
    if (!bgOk && st.bgBuf[0]) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),"(6 hex)"); }

    // Auto-calculate stroke from fill when enabled
    if (st.autoStroke && fillOk) {
        unsigned r,g,b; sscanf(st.fillBuf,"%02x%02x%02x",&r,&g,&b);
        r=(unsigned)(r*0.5f); g=(unsigned)(g*0.5f); b=(unsigned)(b*0.5f);
        snprintf(st.strokeBuf,sizeof(st.strokeBuf),"%02x%02x%02x",r,g,b);
        strokeOk = true;
    }

    // Next requires all three colors to be valid 6-hex values
    bool validHex = isValidHexColor(st.fillBuf) && isValidHexColor(st.strokeBuf) &&
                    isValidHexColor(st.bgBuf);

    if (drawNavButton(ch, false, validHex)) {
        st.page = Page::Music;
    }
}

} // namespace wizard
