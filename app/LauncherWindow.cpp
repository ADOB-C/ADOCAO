#include "LauncherWindow.hpp"

#include "wizard/WizardState.hpp"
#include "wizard/WizardChrome.hpp"
#include "wizard/FileDialogs.hpp"
#include "glad/gl_core.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <cstdio>

// Opens a centered ImGui launcher window. Returns config after user clicks
// Start or closes. The window/GL/ImGui bootstrap lives in wizard::createChrome;
// this function only runs the page loop and dispatches to the per-page draw
// functions in app/wizard/.


namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

LauncherConfig showLauncher() {
    wizard::Chrome chrome;
    if (!wizard::createChrome(chrome)) {
        return {.cancelled = true};
    }

    wizard::State st;
    st.cfg.levelPath.reserve(512);
    st.cfg.musicPath.reserve(512);

    // ---- Main loop ----
    while (!glfwWindowShouldClose(chrome.window) && !st.done) {
        glfwPollEvents();

        // Preload finished → advance to the Hitsounds page
        if (st.preloadRunning && st.preloadFinished.load()) {
            if (st.preloadThread.valid()) {        // ①/④：等价于原 join
                st.preloadThread.wait();
                st.preloadThread.clear();
            }
            st.preloadRunning = false;
            // ② 不再切页（用户已经在 Hitsounds 上了 ✓）：成功就顺手回填音乐，失败只留错误信息
            // （页面右侧会红字显示，Back 可回去改谱 ✓）。
            if (st.cfg.preloadedLevel && st.cfg.preloadedTimeline) {
                if (st.musicBuf[0] == '\0') {
                    std::string autoMusic = wizard::detectMusicFile(st.cfg.levelPath);
                    if (!autoMusic.empty())
                        snprintf(st.musicBuf, sizeof(st.musicBuf), "%s", autoMusic.c_str());
                }
            } else {
                st.lastError = "Failed to load level. Please check the file.";
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Full-window panel
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)chrome.winW, (float)chrome.winH));
        ImGui::Begin("Launcher", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
            // 导航按钮是**绝对定位**在窗口底部的：一旦内容超高一滚动，它们就跟着内容跑
            // （实测截图：Next 被切在窗口底边外 ✗）→ 直接禁掉滚动。
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        switch (st.page) {
            case wizard::Page::Welcome:   wizard::drawWelcomePage(st, chrome);   break;
            case wizard::Page::Hitsounds: wizard::drawHitsoundsPage(st, chrome); break;
            case wizard::Page::Graphics:  wizard::drawGraphicsPage(st, chrome);  break;
            case wizard::Page::Trail:     wizard::drawTrailPage(st, chrome);     break;
            case wizard::Page::Colors:    wizard::drawColorsPage(st, chrome);    break;
            case wizard::Page::Music:     wizard::drawMusicPage(st, chrome);     break;
        }

        ImGui::End();

        // Render
        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(chrome.window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(chrome.window);
    }

    // Ensure preload thread is joined before tearing down
    if (st.preloadRunning && st.preloadThread.valid()) {
        st.preloadThread.wait();
        st.preloadThread.clear();
        st.preloadRunning = false;
    }

    if (!st.done) st.cfg.cancelled = true;

    // ---- Cleanup ----
    wizard::destroyChrome(chrome);

    return st.cfg;
}
