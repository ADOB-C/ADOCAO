#pragma once

// Wizard window + ImGui "chrome": bootstrap/teardown of the launcher window
// (GLFW window, GL context, ImGui context/style/fonts) and the shared
// config-page chrome (progress header + bottom Next/Start button). The page
// loop itself lives in LauncherWindow.cpp.

struct GLFWwindow;
struct ImFont;

namespace wizard {

struct State;   // 定义在 WizardState.hpp

// Runtime chrome for the launcher window + fonts handed to page draw code.
struct Chrome {
    GLFWwindow* window = nullptr;
    int winW = 0, winH = 0;        // window size in logical points
    ImFont* mainFont = nullptr;    // UI font (CJK-capable when available)
    ImFont* titleFont = nullptr;   // large Latin title font (welcome page)
};

// Creates the window, GL context, ImGui context/style and fonts.
// Returns false on failure (window closed / no GL / no fonts fallback ok).
bool createChrome(Chrome& chrome);

// Shuts down ImGui and destroys the window.
void destroyChrome(Chrome& chrome);

// Standard config-page header: progress bar + title + separator.
void drawConfigHeader(const Chrome& chrome, const char* title, float progressPct);

// Bottom navigation button: "Next", or "Start" on the last page (isStart).
// Disabled when canContinue is false. Returns true when the user pressed it.
// 底部导航：Next/Start（居中）+ 可选 Back（在它左侧）。返回 true = 按了 Next/Start；
// `backPressed`（出参，可为 null）= 按了 Back（仅当 showBack 为 true 时绘制）。
bool drawNavButton(const Chrome& chrome, bool isStart, bool canContinue,
                   bool showBack = false, bool* backPressed = nullptr);

// 启动后台预载（解析 + timeline）。抽出来是因为**两个页面**都要能触发它
// （Welcome 的 Next、Hitsounds 页的 Preload 按钮）；已有任务在跑就先取消并汇合，
// 免得两个任务同时写 preloadedLevel/preloadedTimeline（半成品比不预载更糟 ✗）。
void startPreload(State& st);

} // namespace wizard
