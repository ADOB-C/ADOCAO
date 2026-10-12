// 字体链回归：界面里"没显式 PushFont 的控件"也必须能画出 日文假名 / 汉字 / 韩文。
//
// 为什么单独立这个用例（2026-10-11 真踩过两次）：
//  ① 候选表第一支是 macOS 的 `PingFang.ttc`，它 `fopen` 成功、`AddFontFromFileTTF()`
//     却解析失败返回 NULL；旧循环写的是"fopen 成功就 break"，于是它吃掉了整条候选链，
//     后面的 Hiragino / STHeiti / Songti / Arial Unicode 一个都不试 →
//     ImGui 的缺字回退字符是 `?`，`かめりあ` 全渲染成 `????`。
//  ② 就算把 CJK 源合并进了主字体，**没显式 PushFont 的控件**（路径 InputText、Checkbox…）
//     用的是 `io.FontDefault`，它是 NULL 时会退回 ImGui 内建的 ProggyClean（无 CJK 字形）
//     → 照样 `????`。所以本用例断言的是"**默认字体**覆盖 CJK"，而不只是"某支字体覆盖"。
//
// 本用例直接调 app 用的那个函数（`app/FontSetup.hpp::setupUiFonts`），守的就是真实行为：
//   * 至少一支候选能加载（否则 FAIL）；
//   * `io.FontDefault` 指向主字体，且它画得出 あ / か / 中 / 가；
//   * 机器上**一支候选都没有** → SKIP（没装 CJK 字体的 CI 容器不该误报）。

#include "app/FontSetup.hpp"

#include <cmath>
#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

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
    ImGuiIO& io = ImGui::GetIO();
    const adofai::fonts::UiFonts f = adofai::fonts::setupUiFonts(2.0f);   // 与向导同一套（Retina 密度）
    ImFont* latin = f.main;
    ImFont* cjk = f.main;
    const int sources = f.cjkSources;

    std::printf("latin=%s cjk=%s merge 源 %d 支  FontDefault=%s\n", latin ? "OK" : "NULL",
                cjk ? "OK" : "NULL", sources,
                (io.FontDefault == latin) ? "main（✓ 所有控件都会用它）"
                                          : "**不是 main**（✗ 控件会退回内建字体）");

    check(latin != nullptr, "拉丁主字体能加载");
    check(cjk != nullptr && sources > 0,
          "**至少一支 CJK 字体能加载**（否则界面会把中日韩字显示成 ????）");
    // ★ 这一条才是 `????` 的关键：路径 InputText 之类**不显式 PushFont** 的控件用的是
    //   `io.FontDefault`，它必须就是那支带 CJK 源的主字体。
    check(io.FontDefault == latin,
          "io.FontDefault 指向主字体（否则没 PushFont 的控件仍走内建 ProggyClean → 中日韩还是 ?）");

    ImFont* def = io.FontDefault ? io.FontDefault : (io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0]);
    if (def) {
        check(def->IsGlyphInFont(0x3042), "**默认字体**画得出日文平假名 あ（U+3042）");
        check(def->IsGlyphInFont(0x304B), "**默认字体**画得出日文平假名 か（U+304B）—— 路径里的 かめりあ");
        check(def->IsGlyphInFont(0x4E2D), "**默认字体**画得出汉字 中（U+4E2D）");
        check(def->IsGlyphInFont(0xAC00),
              "**默认字体**画得出韩文 가（U+AC00）—— 靠多源合并拿到（Hiragino 没有，Arial Unicode 才有）");
    } else {
        check(false, "拿得到默认字体");
    }

    // PushFont 的语义（1.92 必填 size）——大标题缩水就是踩在这里：
    //   0.0f       = **保持当前字号**（于是"推入大字体"什么也没变 ✗）
    //   LegacySize = 用 AddFontFromFileTTF 时给的字号（1.92 之前的默认行为 ✓）
    ImFont* big = f.title;
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
        check(std::fabs(kept - base) < 0.01f,
              "PushFont(font, 0.0f) = 保持当前字号（大标题缩水就是踩这里）");
        check(std::fabs(used - big->LegacySize) < 1.5f,
              "PushFont(font, font->LegacySize) = 用字体自己的字号（大标题要的是这个）");
    } else {
        std::printf("SKIP: 拿不到字体，跳过 PushFont 语义检查\n");
    }

    ImGui::DestroyContext();
    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
