#pragma once

// 启动器/向导的字体候选表 + "只认真正加载成功"的加载器。
//
// 为什么单独抽出来（2026-10-11 的坑）：两个窗口各抄了一份候选表和加载循环，
// 而循环写的是"**文件 fopen 成功就 break**"—— macOS 的 `PingFang.ttc` 正好
// fopen 成功、`AddFontFromFileTTF()` 却解析失败返回 NULL，于是它**吃掉了整个候选链**，
// 后面的 STHeiti / Songti / Arial Unicode 一个都不试 → 中日韩字符全渲染成 `????`
// （ImGui 的缺字回退字符就是 `?`）。抽到这里之后，两个窗口共用同一个加载器，
// `tests/font_cjk_test.cpp` 也能直接调它做回归（那个用例就是照这个坑写的）。

#include <imgui.h>

#include <cstdio>

namespace adofai {
namespace fonts {

// ---- 候选表（按优先级；平台内建字体，不走 assets）----
inline const char* const* latinCandidates() {
    static const char* kLatin[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
        nullptr,
    };
    return kLatin;
}

inline const char* const* cjkCandidates() {
    static const char* kCjk[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/malgun.ttf",
        "C:/Windows/Fonts/NanumGothic.ttf",
        "C:/Windows/Fonts/gulim.ttc",
        "C:/Windows/Fonts/batang.ttc",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
        "C:/Windows/Fonts/simhei.ttf",
#elif defined(__APPLE__)
        // ⚠️ 顺序有讲究：PingFang 最漂亮，但 ImGui 的字体解析器**读不了这个 .ttc**
        //    （fopen 成功、AddFontFromFileTTF 返回 NULL）。放在第一位只是"万一将来能读"，
        //    真正兜住日文/简中/韩文的是后面这几支 —— 靠 `addFirstAvailableFont()` 依次回退。
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/Supplemental/Songti.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",   // 唯一带韩文的
        "/Library/Fonts/Arial Unicode.ttf",
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
        nullptr,
    };
    return kCjk;
}

// 依次尝试候选路径，返回**第一个真正加载成功**的字体；全失败返回 nullptr。
// `cfg` 可为空；`MergeMode` 之类由调用方在 cfg 里设好。
inline ImFont* addFirstAvailableFont(const char* const* paths, float size, const ImFontConfig* cfg) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    for (int i = 0; paths[i]; ++i) {
        FILE* f = std::fopen(paths[i], "rb");
        if (!f) continue;                    // 文件不存在
        std::fclose(f);
        if (ImFont* font = atlas->AddFontFromFileTTF(paths[i], size, cfg)) return font;   // NULL = 解析失败，继续下一个
    }
    return nullptr;
}

// 把所有**能加载的**候选都当成 merge 源加载进来（返回成功支数）。
//
// 为什么要"全都要"而不是"第一支能用就停"：单支字体往往覆盖不全 —— macOS 的
// Hiragino Sans GB 有假名和汉字、**没有韩文**；Arial Unicode 才带韩文。
// 以前"第一支成功即停"于是韩文还是 `????`。1.92 起字形是**按需光栅化**的，
// 多挂几个源几乎不花钱（没被用到的字形根本不会进图集），所以直接取并集最稳。
inline int mergeAllAvailableFonts(const char* const* paths, float size, const ImFontConfig* cfg) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImFontConfig merge;
    if (cfg) merge = *cfg;
    merge.MergeMode = true;
    int loaded = 0;
    for (int i = 0; paths[i]; ++i) {
        FILE* f = std::fopen(paths[i], "rb");
        if (!f) continue;
        std::fclose(f);
        if (atlas->AddFontFromFileTTF(paths[i], size, &merge)) ++loaded;
    }
    return loaded;
}

}  // namespace fonts
}  // namespace adofai
