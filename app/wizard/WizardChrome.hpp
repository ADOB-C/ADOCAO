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
    ImFont* subtitleFont = nullptr; // page title font (between main and title)
    ImFont* titleFont = nullptr;   // large Latin title font (welcome page)
};

// Creates the window, GL context, ImGui context/style and fonts.
// Returns false on failure (window closed / no GL / no fonts fallback ok).
bool createChrome(Chrome& chrome);

// Shuts down ImGui and destroys the window.
void destroyChrome(Chrome& chrome);

// Standard config-page header: progress bar + title + separator.
void drawConfigHeader(const Chrome& chrome, const char* title, float progressPct);

// ---- 表单栅格（唯一定义处，见 docs/ui-design.md §2）----
// 520x420 的窗口里就这几个数：左右留白 24、标签列 150（控件列 174，子项缩进 24）、行距 8。
// **页面里别再写 140/150/160/300 这种宽度** —— 都在这里算，所有东西的右边缘才会都在
// `winW - kFormPadX` 上（这是"看起来统一"的来源）。
inline constexpr float kFormPadX      = 24.0f;   // 页面左右留白
inline constexpr float kFormLabelW    = 150.0f;  // 标签列宽（控件列 = padX + 150）
inline constexpr float kFormSubIndent = 24.0f;   // 子项整块缩进
inline constexpr float kFormRowGap    = 8.0f;    // 行距
inline constexpr float kFormGap       = 8.0f;    // 同一行内相邻控件间距
inline constexpr float kFormButtonW   = 62.0f;   // 行内小按钮（Choose）
inline constexpr float kFormInlineW   = 150.0f;  // 与复选框同一行的内联控件

// 控件列的 x（`sub` = 挂在上一行复选框下的从属行）
float formCtrlX(bool sub = false);
// 控件宽度：控件列到右留白（喂给 `SetNextItemWidth`）
float formCtrlW(const Chrome& chrome, bool sub = false);
// 画标签，并把光标移到控件列（之后的控件就是这一行的控件）
void formLabel(const Chrome& chrome, const char* label, bool sub = false);
// 左列复选框行（组头或独立开关）；从属于它的行用 `sub = true`
void formCheckbox(const Chrome& chrome, const char* label, bool* value, bool sub = false);
// 统一行距：页面主体前后成对调用（`ItemSpacing = (kFormGap, kFormRowGap)`）
void formStyleBegin();
void formStyleEnd();

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
