#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include "FileDialogs.hpp"
#include <imgui.h>
#include <cstdio>

namespace wizard {

void drawMusicPage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;

    drawConfigHeader(ch, "Music", 100.0f);

    // Auto-detect music once (from parsed level + same-dir files)
    if (!st.musicAutoTried) {
        st.musicAutoTried = true;
        if (st.musicBuf[0] == '\0' && st.cfg.preloadedLevel) {
            std::string autoMusic = detectMusicFile(st.cfg.levelPath);
            if (!autoMusic.empty())
                snprintf(st.musicBuf, sizeof(st.musicBuf), "%s", autoMusic.c_str());
        }
    }

    const float colX = padX;
    float fileW = 300.0f * S;
    float chooseW = 62.0f * S;
    ImGui::SetCursorPosX(colX);
    ImGui::Text("Music file");
    ImGui::Spacing();
    ImGui::SetCursorPosX(colX);
    ImGui::SetNextItemWidth(fileW);
    ImGui::InputText("##music", st.musicBuf, sizeof(st.musicBuf));
    ImGui::SameLine();
    if (ImGui::Button("Choose##mus", ImVec2(chooseW, 0))) {
        auto result = openFileDialog("Select music file", "*.ogg");
        if (!result.empty())
            snprintf(st.musicBuf, sizeof(st.musicBuf), "%s", result.c_str());
    }

    // Start is always available
    if (drawNavButton(ch, true, true)) {
        // Collect everything into cfg and leave the wizard
        st.cfg.musicPath         = st.musicBuf;
        st.cfg.trackFillColor    = st.fillBuf;
        st.cfg.trackStrokeColor  = st.strokeBuf;
        st.cfg.backgroundColor   = st.bgBuf;
        st.cfg.autoStroke        = st.autoStroke;
        st.cfg.enableHitsounds   = st.enableHitsounds;
        st.cfg.forceHitsoundType = st.forceHS ? kHitsoundTypes[st.forceHSIdx] : "";
        st.cfg.legacyCulling     = st.legacyCulling;
        st.cfg.msaaSamples       = kMsaaSamples[st.msaaIdx];
        st.cfg.exclusiveFullscreen = true;
        st.cfg.resolutionW       = kResoW[st.resoIdx];
        st.cfg.resolutionH       = kResoH[st.resoIdx];
        st.cfg.fullscreen        = st.fullscreen;
        st.cfg.showTrail         = st.showTrail;
        st.cfg.trailDuration     = st.trailDuration;
        st.cfg.trailSampleRate   = st.trailSampleRate;
        st.cfg.cancelled         = false;
        st.done = true;
    }
}

} // namespace wizard
