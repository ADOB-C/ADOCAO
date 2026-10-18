#include "app/HitsoundTypes.hpp"
#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include "FileDialogs.hpp"
#include <imgui.h>
#include <array>
#include <cstdio>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

// 版面走 WizardChrome 的表单栅格（docs/ui-design.md）：标签在左列，输入框 + Choose 占满控件列，
// 按钮右边缘与页头/分隔线对齐。
void drawMusicPage(State& st, const Chrome& ch) {
    formStyleBegin();
    drawConfigHeader(ch, "Music", 100.0f);

    // Auto-detect music once (from parsed level + same-dir files)
    formLabel(ch, "Music file");
    const float rowW = formCtrlW(ch);
    ImGui::SetNextItemWidth(rowW - kFormButtonW - kFormGap);   // 输入框 + 间距 + 按钮 = 控件列宽
    ImGui::InputText("##music", st.musicBuf, sizeof(st.musicBuf));
    ImGui::SameLine(0.0f, kFormGap);
    if (ImGui::Button("Choose##mus", ImVec2(kFormButtonW, 0))) {
        auto result = openFileDialog("Select music file",
                                     {"*.ogg", "*.mp3", "*.wav", "*.flac", "*.m4a", "*.aiff"},
                                     "Audio files");
        if (!result.empty())
            snprintf(st.musicBuf, sizeof(st.musicBuf), "%s", result.c_str());
    }

    formStyleEnd();

    // Start is always available
    bool back = false;
    // Back 时 drawNavButton 返回 false（只有 `back` 出参知道按下去了）→ 必须 `|| back`
    if (drawNavButton(ch, true, /*canContinue=*/true, /*showBack=*/true, &back) || back) {
        if (back) { st.page = prevPage(st.page); return; }   // Music → Colors
        // Collect everything into cfg and leave the wizard
        st.cfg.musicPath         = st.musicBuf;
        // Colors are edited as float RGB; commit them in the config's hex form.
        // When auto stroke is on the stroke is derived here as well, so the
        // result does not depend on the Colors page having been drawn.
        st.cfg.trackFillColor    = hexFromRgb(st.fillColor.data());
        const std::array<float, 3> stroke = st.autoStroke
                                          ? deriveStroke(st.fillColor.data())
                                          : st.strokeColor;
        st.cfg.trackStrokeColor  = hexFromRgb(stroke.data());
        st.cfg.backgroundColor   = hexFromRgb(st.bgColor.data());
        st.cfg.autoStroke        = st.autoStroke;
        st.cfg.enableHitsounds   = st.enableHitsounds;
        st.cfg.forceHitsoundType = st.forceHS ? kHitsoundTypes[st.forceHSIdx] : "";
        st.cfg.msaaSamples       = kMsaaSamples[st.msaaIdx];
        st.cfg.exclusiveFullscreen = st.exclusive;
        st.cfg.resolutionW       = kResoW[st.resoIdx];
        st.cfg.resolutionH       = kResoH[st.resoIdx];
        st.cfg.fullscreen        = st.fullscreen;
        st.cfg.cullBlockFloors   = kCullBlockValues[st.cullBlockIdx];
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
