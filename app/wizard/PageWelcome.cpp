#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include "FileDialogs.hpp"
#include "app/LevelLoader.hpp"
#include <imgui.h>
#include <cstdio>
#include <mutex>
#include <thread>

namespace wizard {

void drawWelcomePage(State& st, const Chrome& ch) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const int winW = ch.winW, winH = ch.winH;
    const float btnW = 110.0f * S;
    const float btnH = 30.0f * S;

    // Big centered title
    float titleY = 70.0f * S;
    ImGui::SetCursorPosY(titleY);
    const char* title = "Welcome to ADOCAO";
    if (ch.titleFont) ImGui::PushFont(ch.titleFont);
    ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorPosX((float)(winW - (int)ts.x) / 2.0f);
    ImGui::Text("%s", title);
    if (ch.titleFont) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Level file picker (path + Choose), centered as a group
    float fileW = 300.0f * S;
    float chooseW = 62.0f * S;
    float gap = 8.0f * S;
    float groupW = fileW + gap + chooseW;
    float x0 = ((float)winW - groupW) / 2.0f;

    ImGui::SetCursorPosY(titleY + 46.0f * S);
    ImGui::SetCursorPosX(x0);
    ImGui::SetNextItemWidth(fileW);
    ImGui::InputText("##level", st.levelBuf, sizeof(st.levelBuf));
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("Choose", ImVec2(chooseW, 0))) {
        auto result = openFileDialog("Select level file",
                                     {"*.adofai", "*.adofai.xz", "*.adofai.zst"},
                                     "ADOFAI levels (plain / xz / zstd)");
        if (!result.empty()) {
            snprintf(st.levelBuf, sizeof(st.levelBuf), "%s", result.c_str());
            st.lastError.clear();
        }
    }

    // Preload checkbox, left edge aligned with the file box
    ImGui::SetCursorPosX(x0);
    ImGui::Checkbox("Preload", &st.preloadEnabled);
    ImGui::Spacing();
    if (!st.lastError.empty()) {
        ImGui::SetCursorPosX(x0);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", st.lastError.c_str());
    }

    // Next / Export buttons centered
    ImGui::SetCursorPosY((float)winH - 56.0f * S);
    float xb = ((float)winW - (btnW * 2.0f + 12.0f * S)) / 2.0f;
    ImGui::SetCursorPosX(xb);
    bool levelOk = st.levelBuf[0] != '\0';
    if (!levelOk) ImGui::BeginDisabled();
    if (ImGui::Button("Next", ImVec2(btnW, btnH))) {
        if (levelOk) {
            st.cfg.levelPath = st.levelBuf;
            st.lastError.clear();
            if (st.preloadEnabled) {
                // Start background preload (parse + timeline)
                st.preloadProgress.percent.store(0.0f);
                st.preloadProgress.stage.store(0);
                {
                    std::lock_guard<std::mutex> lk(st.preloadProgress.textMutex);
                    snprintf(st.preloadProgress.stageText, sizeof(st.preloadProgress.stageText),
                             "Starting...");
                }
                st.preloadFinished.store(false);
                st.preloadRunning = true;
                st.cfg.preloadedLevel.reset();
                st.cfg.preloadedTimeline.reset();
                st.preloadThread = std::thread([&st]() {
                    runLevelPreload(st.cfg, st.preloadProgress,
                                    st.cfg.preloadedLevel, st.cfg.preloadedTimeline);
                    st.preloadFinished.store(true);
                });
                st.page = Page::Preload;
            } else {
                st.page = Page::Hitsounds;
            }
        }
    }
    ImGui::SameLine(0.0f, 12.0f * S);
    if (ImGui::Button("Export", ImVec2(btnW, btnH))) {
        if (levelOk) {
            // Choose export directory, default = level's folder
            std::string defDir;
            {
                std::string lvl(st.levelBuf);
                auto slash = lvl.find_last_of("/\\");
                defDir = slash == std::string::npos ? "." : lvl.substr(0, slash);
            }
            std::string dir = selectFolderDialog("Select export folder", defDir);
            if (!dir.empty()) {
                st.cfg.levelPath = st.levelBuf;
                st.cfg.exportDir = dir;
                st.cfg.enableHitsounds = true;
                st.cfg.exportHitsounds = true;
                st.cfg.cancelled = false;
                st.done = true;
            }
        }
    }
    if (!levelOk) ImGui::EndDisabled();
}

} // namespace wizard
