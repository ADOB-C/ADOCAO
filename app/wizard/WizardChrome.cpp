#include "core/util/Scheduler.hpp"   // startPreload
#include "WizardChrome.hpp"
#include "WizardState.hpp"
#include "app/LevelLoader.hpp"   // runLevelPreload（startPreload 用）

#include "app/FontSetup.hpp"           // 字体候选表 + "只认真正加载成功"的加载器
#include "core/util/Logger.hpp"     // CJK 字体加载失败要喊一声（否则只看到 `????`）
#include "glad/gl_core.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <cstdio>
#include <mutex>
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
    // Fonts：正文（含 CJK 合并源）+ 大标题（1.8x）；主字体同时设为默认字体
    const fonts::UiFonts f = fonts::setupUiFonts(dpiScale);
    ImFont* mainFont = f.main;
    ImFont* titleFont = f.title;
    if (f.cjkSources == 0) LOG_W("CJK 字体一个都没加载成功，中日韩字符会显示成 `????`");

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

bool drawNavButton(const Chrome& chrome, bool isStart, bool canContinue,
                   bool showBack, bool* backPressed) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float btnW = 110.0f * S;
    const float btnH = 30.0f * S;

    if (backPressed) *backPressed = false;
    ImGui::SetCursorPosY((float)chrome.winH - 56.0f * S);
    // Back 在 Next 左侧 12px；窗口比两个按钮宽得多，不会挤到标题栏
    if (showBack) {
        const float xb = ((float)chrome.winW - btnW) / 2.0f - btnW - 12.0f * S;
        ImGui::SetCursorPosX(xb);
        if (ImGui::Button("Back", ImVec2(btnW, btnH)) && backPressed) *backPressed = true;
    }
    // ⚠️ 必须**同时**重设 Y：画完 Back 后光标已经往下走了一个按钮高，
    // 只重设 X 会让 Next 掉到 Back 下面（实测截图：Next 被切在窗口底边外 ✗）。
    ImGui::SetCursorPosY((float)chrome.winH - 56.0f * S);
    float xb = ((float)chrome.winW - btnW) / 2.0f;
    ImGui::SetCursorPosX(xb);
    if (!canContinue) ImGui::BeginDisabled();
    const char* label = isStart ? "Start" : "Next";
    bool pressed = ImGui::Button(label, ImVec2(btnW, btnH));
    if (!canContinue) ImGui::EndDisabled();
    return pressed;
}

void startPreload(State& st) {
    // 已有任务在跑：先取消 + 汇合，否则两个任务会同时写 preloadedLevel/Timeline ✗
    if (st.preloadRunning && st.preloadThread.valid()) {
        st.preloadThread.cancel();
        st.preloadThread.wait();
        st.preloadThread.clear();
    }
    st.preloadProgress.percent.store(0.0f);
    st.preloadProgress.stage.store(0);
    {
        std::lock_guard<std::mutex> lk(st.preloadProgress.textMutex);
        snprintf(st.preloadProgress.stageText, sizeof(st.preloadProgress.stageText), "Starting...");
    }
    st.preloadFinished.store(false);
    st.preloadRunning = true;
    st.cfg.preloadedLevel.reset();
    st.cfg.preloadedTimeline.reset();
    st.lastError.clear();
    st.preloadThread = adofai::Scheduler::instance().submit(
        adofai::Lane::Background, [&st](adofai::Scheduler::CancelToken) {
            runLevelPreload(st.cfg, st.preloadProgress, st.cfg.preloadedLevel, st.cfg.preloadedTimeline);
            st.preloadFinished.store(true);
        });
}

} // namespace wizard
