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
    // Fonts：正文（含 CJK 合并源）+ 页小标题（1.4x）+ 大标题（1.8x）；主字体同时设为默认字体
    const fonts::UiFonts f = fonts::setupUiFonts(dpiScale);
    ImFont* mainFont = f.main;
    ImFont* subtitleFont = f.subtitle;
    ImFont* titleFont = f.title;
    if (f.cjkSources == 0) LOG_W("CJK 字体一个都没加载成功，中日韩字符会显示成 `????`");

    // 1.92 起不需要 FontGlobalScale：逻辑字号 + RasterizerDensity 已经是"布局按逻辑、字形按密度"。

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    chrome.window = window;
    chrome.winW = winW;
    chrome.winH = winH;
    chrome.mainFont = mainFont;
    chrome.subtitleFont = subtitleFont;
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
    // 高度用 ImGui 的标准框高（字号 + 上下 FramePadding），**别自己压到 10px** ✗：
    // ImGui 的百分比文字是**裁剪到进度条矩形**里的（imgui_widgets.cpp 的 RenderTextClipped
    // 以 bb 为 clip_rect），条比字体矮就只画出中间一条，文字被压扁（用户实测截图）。
    ImGui::ProgressBar(progressPct / 100.0f, ImVec2(contentW, ImGui::GetFrameHeight()));
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::SetCursorPosX(padX);
    // 页小标题：比正文大一档、又明显小于 Welcome 的 "Welcome to ADOCAO"（1.4x vs 1.8x）。
    // 1.92 起 PushFont 的 size 必填，`LegacySize` = `AddFontFromFileTTF` 时给的字号 ✓
    // （传 0.0f 是"保持当前字号"，等于什么都没变 ✗ —— 见 FontSetup.hpp 的注释）。
    if (chrome.subtitleFont) ImGui::PushFont(chrome.subtitleFont, chrome.subtitleFont->LegacySize);
    ImGui::Text("%s", title);
    if (chrome.subtitleFont) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::SetCursorPosX(padX);
    ImGui::Separator();
    // 分隔线下面只留一个 Spacing：页标题换成 1.4x 字体后这一栏高了 ~5px，而 Visuals 页
    // （最长的一页，最后一行 Background）离底部按钮原本就只剩 ~13px —— 多了这 5px 就**压到按钮上**
    // （实测截图：Background 的输入框被 Back/Next 盖住）。从这里的空白补回来，别动页内容。
    ImGui::Spacing();
}

bool drawNavButton(const Chrome& chrome, bool isStart, bool canContinue,
                   bool showBack, bool* backPressed) {
    const float S = 1.0f;  // hardcoded sizes are in logical pixels
    const float btnW = 110.0f * S;
    const float btnH = 30.0f * S;

    if (backPressed) *backPressed = false;
    // 两个按钮时按**一对居中**摆（与 Welcome 页 `Next`+`Export` 同一约定 ✓），
    // 单个时维持原来的"居中" ✓。间距 12px 用 `SameLine`（与 Welcome 页一致 ✓）。
    ImGui::SetCursorPosY((float)chrome.winH - 56.0f * S);
    const float pairW = btnW * 2.0f + 12.0f * S;
    float xb = showBack ? ((float)chrome.winW - pairW) / 2.0f
                        : ((float)chrome.winW - btnW) / 2.0f;
    ImGui::SetCursorPosX(xb);
    bool backClicked = false;
    if (showBack) {
        backClicked = ImGui::Button("Back", ImVec2(btnW, btnH));
        ImGui::SameLine(0.0f, 12.0f * S);
    }
    if (!canContinue) ImGui::BeginDisabled();
    const char* label = isStart ? "Start" : "Next";
    bool pressed = ImGui::Button(label, ImVec2(btnW, btnH));
    if (!canContinue) ImGui::EndDisabled();
    if (backClicked && backPressed) *backPressed = true;
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
