#include "WizardChrome.hpp"

#include "glad/gl_core.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <algorithm>
#include <cstdio>

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

    // High-DPI fonts: rasterize glyphs at physical resolution (Retina = 2x),
    // then scale layout back to logical size via FontGlobalScale.
    float dpiScale = 1.0f;
    {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        if (mon) {
            float sx, sy;
            glfwGetMonitorContentScale(mon, &sx, &sy);
            dpiScale = std::max(sx, sy);
        }
    }
    float fontSize = 12.0f * dpiScale;

    // Fonts: main (with CJK merge) + a larger title font (Latin only)
    ImFont* mainFont = nullptr;
    ImFont* titleFont = nullptr;
    {
        ImGuiIO& io = ImGui::GetIO();
        const char* cjkFonts[] = {
#ifdef _WIN32
            "C:/Windows/Fonts/malgun.ttf",
            "C:/Windows/Fonts/NanumGothic.ttf",
            "C:/Windows/Fonts/gulim.ttc",
            "C:/Windows/Fonts/batang.ttc",
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/msgothic.ttc",
            "C:/Windows/Fonts/simhei.ttf",
#elif defined(__APPLE__)
            "/System/Library/Fonts/PingFang.ttc",
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/System/Library/Fonts/Supplemental/Songti.ttc",
#else
            "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/nanum/NanumGothic.ttf",
            "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
#endif
        };
        static const ImWchar cjkRanges[] = {
            0x2000, 0x206F, 0x3000, 0x303F, 0x3040, 0x309F,
            0x30A0, 0x30FF, 0x3130, 0x318F, 0x4E00, 0x9FFF,
            0xAC00, 0xD7AF, 0xFF00, 0xFFEF, 0,
        };

        // Main Latin font
        {
            const char* latinFonts[] = {
#ifdef _WIN32
                "C:/Windows/Fonts/segoeui.ttf",
                "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
                "/System/Library/Fonts/Helvetica.ttc",
                "/System/Library/Fonts/SFNS.ttf",
#endif
            };
            bool latinLoaded = false;
            for (const char* path : latinFonts) {
                FILE* f = fopen(path, "rb");
                if (f) { fclose(f); mainFont = io.Fonts->AddFontFromFileTTF(path, fontSize); latinLoaded = true; break; }
            }
            if (!latinLoaded) mainFont = io.Fonts->AddFontDefault();
        }

        // Title font (bigger, Latin only)
        {
            const char* latinFonts[] = {
#ifdef _WIN32
                "C:/Windows/Fonts/segoeui.ttf",
                "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
                "/System/Library/Fonts/Helvetica.ttc",
                "/System/Library/Fonts/SFNS.ttf",
#endif
            };
            for (const char* path : latinFonts) {
                FILE* f = fopen(path, "rb");
                if (f) { fclose(f); titleFont = io.Fonts->AddFontFromFileTTF(path, fontSize * 1.8f); break; }
            }
            if (!titleFont) titleFont = mainFont;
        }

        // Merge CJK into the main font
        ImFontConfig cfg;
        cfg.MergeMode = true;
        for (const char* path : cjkFonts) {
            FILE* f = fopen(path, "rb");
            if (f) { fclose(f); io.Fonts->AddFontFromFileTTF(path, fontSize, &cfg, cjkRanges); break; }
        }
    }

    // Scale layout back to logical size; glyphs stay crisp at physical res.
    ImGui::GetIO().FontGlobalScale = 1.0f / dpiScale;

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
