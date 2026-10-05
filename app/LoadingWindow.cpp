#include "LoadingWindow.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <thread>
#include <mutex>
#include <cstdio>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

static constexpr int LOADING_W = 480;
static constexpr int LOADING_H = 160;

void showLoadingWindow(std::function<void(LoadingProgress&)> loader) {
    // Window size in logical points. The GLFW + ImGui backends handle high-DPI
    // (Retina) scaling automatically via DisplayFramebufferScale — manually
    // multiplying by monitor content scale would double-scale and blur on macOS.
    int winW = LOADING_W;
    int winH = LOADING_H;

    // ---- Create window ----
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(winW, winH, "ADOCAO - Loading", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create loading window\n");
        return;
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

    // DPI-aware sizing: scale all UI elements, load fonts at native resolution
    // High-DPI fonts: rasterize glyphs at physical resolution (Retina = 2x),
    // then scale layout back to logical size via FontGlobalScale (ImGui DPI
    // guidelines). Window size stays in logical points.
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

    // Load CJK font
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
                if (f) { fclose(f); io.Fonts->AddFontFromFileTTF(path, fontSize); latinLoaded = true; break; }
            }
            if (!latinLoaded) io.Fonts->AddFontDefault();
        }
        ImFontConfig cfg;
        cfg.MergeMode = true;
        bool loaded = false;
        for (const char* path : cjkFonts) {
            FILE* f = fopen(path, "rb");
            if (f) { fclose(f); io.Fonts->AddFontFromFileTTF(path, fontSize, &cfg, cjkRanges); loaded = true; break; }
        }
    }

    // Scale layout back to logical size; glyphs stay crisp at physical res.
    ImGui::GetIO().FontGlobalScale = 1.0f / dpiScale;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // ---- Start loader thread ----
    LoadingProgress progress;
    std::thread loaderThread([&]() {
        loader(progress);
        progress.percent.store(100.0f);
    });

    // ---- Render loop ----
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Full-window panel
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("Loading", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        float S = 1.0f;  // hardcoded sizes are in logical pixels

        ImGui::Spacing();
        ImGui::Text("Loading...");
        ImGui::Separator();
        ImGui::Spacing();

        float pct = progress.percent.load();
        ImGui::SetNextItemWidth(440 * S);
        ImGui::ProgressBar(pct / 100.0f, ImVec2(0, 24 * S));

        ImGui::Spacing();
        {
            std::lock_guard<std::mutex> lock(progress.textMutex);
            ImGui::Text("%s", progress.stageText);
        }

        ImGui::End();

        // Render
        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        // Close when loader finishes
        if (pct >= 100.0f && !loaderThread.joinable()) {
            break;
        }
        if (pct >= 100.0f && loaderThread.joinable()) {
            loaderThread.join();
            break;
        }
    }

    // Ensure thread is joined
    if (loaderThread.joinable()) {
        loaderThread.join();
    }

    // ---- Cleanup ----
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
}
