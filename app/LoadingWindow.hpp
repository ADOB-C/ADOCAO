#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <mutex>

// Loading progress reporter passed to the loading callback
struct LoadingProgress {
    std::atomic<float> percent{0.0f};  // 0.0 - 100.0
    std::atomic<int>   stage{0};
    char stageText[256] = "Initializing...";
    std::mutex textMutex;  // guards stageText (written from loader thread, read from UI thread)
};

// Shows a centered progress window. Calls loader() in a background thread,
// updates progress bar until loader finishes, then closes.
// loader receives LoadingProgress& that it should update.
// cliOnly = true：**不建任何窗口、不碰 ImGui**，把同一份进度打成 CLI 进度行（stderr）。
// 非交互运行（--capture 等脚本化/验收运行）用这个，免得每次都在屏幕上弹一个窗口。
void showLoadingWindow(std::function<void(LoadingProgress&)> loader, bool cliOnly = false);
