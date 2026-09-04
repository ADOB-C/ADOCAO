#pragma once

// Wizard window + ImGui "chrome": bootstrap/teardown of the launcher window
// (GLFW window, GL context, ImGui context/style/fonts) and the shared
// config-page chrome (progress header + bottom Next/Start button). The page
// loop itself lives in LauncherWindow.cpp.

struct GLFWwindow;
struct ImFont;

namespace wizard {

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
bool drawNavButton(const Chrome& chrome, bool isStart, bool canContinue);

} // namespace wizard
