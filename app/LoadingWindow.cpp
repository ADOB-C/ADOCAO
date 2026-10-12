#include "LoadingWindow.hpp"
#include "core/util/Progress.hpp"
#include "app/FontSetup.hpp"
#include "core/util/Logger.hpp"
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

void showLoadingWindow(std::function<void(LoadingProgress&)> loader, bool cliOnly) {
    if (cliOnly) {
        // 非交互运行（--capture 等）：**不建窗口、不碰 ImGui**，把同一份进度打成 CLI 进度行
        // （stderr、\r 原地刷新、速率+ETA，见 core/util/Progress.hpp）。加载照样在后台线程里跑，
        // 进度结构与 GUI 那条路完全一样，所以下面的行为不受影响。
        LoadingProgress progress;
        std::thread loaderThread([&]() {
            loader(progress);
            progress.percent.store(100.0f);
        });
        progress::reset();
        while (progress.percent.load() < 100.0f) {
            {   // stageText 由 loader 线程写，读的时候持锁（update 只是拷贝字符串，可以放在锁内）
                std::lock_guard<std::mutex> lock(progress.textMutex);
                progress::update((long long)progress.percent.load(), 100, "%", progress.stageText);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        progress::update(100, 100, "%", "done");
        progress::finish();
        if (loaderThread.joinable()) loaderThread.join();
        return;
    }

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
    // High-DPI fonts（1.92 写法：字号用逻辑尺寸 + ImFontConfig::RasterizerDensity 决定清晰度；
    // 旧的"按物理像素烘字体 + FontGlobalScale 反缩放"已废弃 —— 见 ImGui 1.92 迁移说明）。
    // Window size stays in logical points.
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
    // Fonts：正文（含 CJK 合并源）；主字体同时设为默认字体（否则没 PushFont 的控件看不到 CJK）
    const fonts::UiFonts f = fonts::setupUiFonts(dpiScale);
    if (f.cjkSources == 0) LOG_W("CJK 字体一个都没加载成功，中日韩字符会显示成 `????`");

    // 1.92 起不需要 FontGlobalScale：逻辑字号 + RasterizerDensity 已经是"布局按逻辑、字形按密度"。

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
