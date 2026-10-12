// 字体链回归：**必须至少有一支 CJK 字体真的能加载并覆盖 日文假名 / 汉字 / 韩文**。
//
// 为什么单独立这个用例（2026-10-11 真踩过）：启动器/向导的候选表里第一个是 macOS 的
// `PingFang.ttc`，它 `fopen` 成功、`AddFontFromFileTTF()` 却解析失败返回 NULL；
// 旧循环写的是"fopen 成功就 break"，于是它吃掉了整条候选链，后面的
// Hiragino / STHeiti / Songti / Arial Unicode 一个都不试 → 路径里的 `かめりあ`
// 全渲染成 `????`（ImGui 的缺字回退字符就是 `?`），而且**只在带中日韩字符的谱上看得见**。
//
// 本用例直接调用 app 用的那个加载器（`app/FontSetup.hpp`），所以它守的就是真实行为：
//   * 至少一支候选能加载（否则 FAIL —— 本轮修的就是这个）；
//   * 它能画出 あ / か / 中 / 가（缺哪个就在哪一项上报，方便换字体）；
//   * 机器上**一支候选都没有** → SKIP（比如没装 CJK 字体的 CI 容器，不该误报）。

#include "app/FontSetup.hpp"

#include <cmath>
#include <cstdio>


int main() {
    // 先看看这台机器上有哪些候选文件真实存在
    int present = 0;
    for (int i = 0; adofai::fonts::cjkCandidates()[i]; ++i) {
        if (FILE* f = std::fopen(adofai::fonts::cjkCandidates()[i], "rb")) {
            std::fclose(f);
            ++present;
            std::printf("候选存在: %s\n", adofai::fonts::cjkCandidates()[i]);
        }
    }
    if (present == 0) {
        std::printf("SKIP: 这台机器上一支候选 CJK 字体都没有（CI 容器等）\n");
        return 0;
    }

    ImGui::CreateContext();
    ImFontConfig cfg;
    cfg.MergeMode = true;                       // 与向导一致：合并到主字体上
    ImFont* latin = adofai::fonts::addFirstAvailableFont(adofai::fonts::latinCandidates(), 12.0f, nullptr);
    const int sources = adofai::fonts::mergeAllAvailableFonts(adofai::fonts::cjkCandidates(), 12.0f, &cfg);
    ImFont* cjk = ImGui::GetIO().Fonts->Fonts.empty() ? nullptr : ImGui::GetIO().Fonts->Fonts[0];

    int fail = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fail;
    };
    std::printf("latin=%s cjk=%s merge 源 %d 支\n", latin ? "OK" : "NULL", cjk ? "OK" : "NULL", sources);
    check(latin != nullptr, "拉丁主字体能加载");
    check(cjk != nullptr && sources > 0,
          "**至少一支 CJK 字体能加载**（否则界面会把中日韩字显示成 ????）");
    if (cjk) {
        check(cjk->IsGlyphInFont(0x3042), "有日文平假名 あ（U+3042）");
        check(cjk->IsGlyphInFont(0x304B), "有日文平假名 か（U+304B）");
        check(cjk->IsGlyphInFont(0x4E2D), "有汉字 中（U+4E2D）");
        check(cjk->IsGlyphInFont(0xAC00),
              "有韩文 가（U+AC00）—— 靠多源合并拿到的（Hiragino 没有，Arial Unicode 才有）");
    }
    // PushFont 的语义（1.92 必填 size）——大标题缩水就是踩在这里：
    //   0.0f       = **保持当前字号**（于是"推入大字体"什么也没变 ✗）
    //   LegacySize = 用 AddFontFromFileTTF 时给的字号（1.92 之前的默认行为 ✓）
    if (latin == nullptr) latin = ImGui::GetIO().Fonts->AddFontDefault();
    ImFont* big = nullptr;
    {
        FILE* f = std::fopen(adofai::fonts::latinCandidates()[0], "rb");
        if (f) { std::fclose(f); big = ImGui::GetIO().Fonts->AddFontFromFileTTF(adofai::fonts::latinCandidates()[0], 21.6f); }
    }
    if (big && latin) {
        // 先显式推一个已知字号当基线（没跑过 NewFrame 时 g.FontSize 还是 0，拿它当基线没有意义）
        ImGui::PushFont(latin, 12.0f);
        const float base = ImGui::GetFontSize();
        ImGui::PushFont(big, 0.0f);
        const float kept = ImGui::GetFontSize();
        ImGui::PopFont();
        ImGui::PushFont(big, big->LegacySize);
        const float used = ImGui::GetFontSize();
        ImGui::PopFont();
        ImGui::PopFont();
        std::printf("PushFont: 基线(12)=%.2f  0.0f→%.2f  LegacySize(%.2f)→%.2f\n", base, kept,
                    big->LegacySize, used);
        check(std::fabs(base - 12.0f) < 0.01f, "显式 PushFont(font, 12) 基线与请求一致");
        check(std::fabs(kept - base) < 0.01f, "PushFont(font, 0.0f) = 保持当前字号（大标题缩水就是踩这里）");
        check(std::fabs(used - big->LegacySize) < 1.5f,
              "PushFont(font, font->LegacySize) = 用字体自己的字号（大标题要的是这个）");
    } else {
        std::printf("SKIP: 拿不到拉丁字体，跳过 PushFont 语义检查\n");
    }

    ImGui::DestroyContext();
    if (fail) {
        std::printf("\n%d 项失败\n", fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
