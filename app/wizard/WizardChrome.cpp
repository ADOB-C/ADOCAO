#include "WizardChrome.hpp"

#include "app/FontSetup.hpp"           // 字体候选表 + "只认真正加载成功"的加载器
#include "core/util/Logger.hpp"     // CJK 字体加载失败要喊一声（否则只看到 `????`）
#include "glad/gl_core.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <algorithm>
#include <cstdio>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

namespace {
constexpr int LAUNCHER_W = 520;
constexpr int LAUNCHER_H = 420;
}

bool createChrome(Chrome& chrome) {
    // Window size in logical points. The GLFW + ImGui backends handle high-DPI
    // (Retina) scaling automatically via DisplayFramebufferScale.
    int winW = LAUNCHER_W;
    int winH = LAUNCHER_H;

    // ---- Create window ----
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(winW, winH, "ADOCAO", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create launcher window\n");
        return false;
    }

    // Center on primary monitor (work area in logical points)
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        int wx, wy, ww, wh;
        glfwGetMonitorWorkarea(monitor, &wx, &wy, &ww, &wh);
        glfwSetWindowPos(window, wx + (ww - winW) / 2, wy + (wh - winH) / 2);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Load GL function pointers (glad). The wizard renders via ImGui/GL right
    // away, before GameWindow::init would normally call loadGLCore().
    if (!loadGLCore()) {
        fprintf(stderr, "Failed to load OpenGL functions\n");
        glfwDestroyWindow(window);
        return false;
    }

    // ---- Init ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    {   // Rounded corners (radius 4)
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding    = 4.0f;
        st.FrameRounding     = 4.0f;
        st.PopupRounding     = 4.0f;
        st.ChildRounding     = 4.0f;
        st.ScrollbarRounding = 4.0f;
        st.GrabRounding      = 4.0f;
        st.TabRounding       = 4.0f;
    }

    // High-DPI fonts: 字号用**逻辑**尺寸，字形按 RasterizerDensity（Retina = 2）光栅化 ——
    // 布局与清晰度就解耦了（1.92 起不再需要 FontGlobalScale 那套反缩放）。
    float dpiScale = 1.0f;
    {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        if (mon) {
            float sx, sy;
            glfwGetMonitorContentScale(mon, &sx, &sy);
            dpiScale = std::max(sx, sy);
        }
    }
    // 1.92：字号是**逻辑**尺寸，清晰度由 ImFontConfig::RasterizerDensity 决定
    // （旧写法是"按物理像素烘字体 + FontGlobalScale 反缩放回逻辑尺寸"，已废弃）
    float fontSize = 12.0f;
    ImFontConfig baseCfg;
    baseCfg.RasterizerDensity = dpiScale;

    // Fonts: main (with CJK merge) + a larger title font (Latin only)
    ImFont* mainFont = nullptr;
    ImFont* titleFont = nullptr;
    {
        ImGuiIO& io = ImGui::GetIO();

        // Main Latin font
        {
            mainFont = fonts::addFirstAvailableFont(fonts::latinCandidates(), fontSize, &baseCfg);
            if (!mainFont) mainFont = io.Fonts->AddFontDefault(&baseCfg);
        }

        // Title font (bigger, Latin only)
        {
            titleFont = fonts::addFirstAvailableFont(fonts::latinCandidates(), fontSize * 1.8f, &baseCfg);
            if (!titleFont) titleFont = mainFont;
        }

        // Merge CJK into the main font
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.RasterizerDensity = dpiScale;      // 1.92：字形按需光栅化，不用再预烘 CJK 区段
        const int cjkSources = fonts::mergeAllAvailableFonts(fonts::cjkCandidates(), fontSize, &cfg);
        if (cjkSources == 0) LOG_W("CJK 字体一个都没加载成功，中日韩字符会显示成 `????`");
    }

    // 1.92 起不需要 FontGlobalScale：逻辑字号 + RasterizerDensity 已经是"布局按逻辑、字形按密度"。

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    chrome.window = window;
    chrome.winW = winW;
    chrome.winH = winH;
    chrome.mainFont = mainFont;
    chrome.titleFont = titleFont;
    return true;
}

void destroyChrome(Chrome& chrome) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(chrome.window);
    chrome.window = nullptr;
}

void drawConfigHeader(const Chrome& chrome, const char* title, float progressPct) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float padX = 24.0f * S;
    const float contentW = (float)chrome.winW - 2.0f * padX;

    ImGui::SetCursorPosY(18.0f * S);
    ImGui::SetCursorPosX(padX);
    ImGui::ProgressBar(progressPct / 100.0f, ImVec2(contentW, 10.0f * S));
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::SetCursorPosX(padX);
    ImGui::Text("%s", title);
    ImGui::Spacing();
    ImGui::SetCursorPosX(padX);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Spacing();
}

bool drawNavButton(const Chrome& chrome, bool isStart, bool canContinue) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float btnW = 110.0f * S;
    const float btnH = 30.0f * S;

    ImGui::SetCursorPosY((float)chrome.winH - 56.0f * S);
    float xb = ((float)chrome.winW - btnW) / 2.0f;
    ImGui::SetCursorPosX(xb);
    if (!canContinue) ImGui::BeginDisabled();
    const char* label = isStart ? "Start" : "Next";
    bool pressed = ImGui::Button(label, ImVec2(btnW, btnH));
    if (!canContinue) ImGui::EndDisabled();
    return pressed;
}

} // namespace wizard
