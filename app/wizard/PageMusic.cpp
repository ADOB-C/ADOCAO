#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include "FileDialogs.hpp"
#include <imgui.h>
#include <array>
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
        // Colors are edited as float RGB; commit them in the config's hex form.
        // When auto stroke is on the stroke is derived here as well, so the
        // result does not depend on the Visuals page having been drawn.
        st.cfg.trackFillColor    = hexFromRgb(st.fillColor.data());
        const std::array<float, 3> stroke = st.autoStroke
                                          ? deriveStroke(st.fillColor.data())
                                          : st.strokeColor;
        st.cfg.trackStrokeColor  = hexFromRgb(stroke.data());
        st.cfg.backgroundColor   = hexFromRgb(st.bgColor.data());
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
        st.cfg.trailAdaptive     = false;   // wizard sliders = manual fixed rate
        st.cfg.trailPerTile      = st.trailPerTile;
        st.cfg.trailSamplesPerTile = st.trailSamplesPerTile;
        st.cfg.trailLengthInTiles = st.trailLengthInTiles;
        st.cfg.trailTiles        = st.trailTiles;
        st.cfg.cancelled         = false;
        st.done = true;
    }
}

} // namespace wizard
