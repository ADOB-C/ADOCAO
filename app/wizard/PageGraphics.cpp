#include "WizardState.hpp"
#include "WizardChrome.hpp"
#include <imgui.h>

namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace wizard {

// 版面全部走 WizardChrome 的表单栅格（docs/ui-design.md）：标签在左列、控件在控件列、
// 右边距与页头一致。这一页**不写任何坐标/宽度数字**。
void drawGraphicsPage(State& st, const Chrome& ch) {
    formStyleBegin();
    drawConfigHeader(ch, "Graphics", 40.0f);

    formLabel(ch, "Resolution");
    ImGui::SetNextItemWidth(formCtrlW(ch));
    ImGui::Combo("##reso", &st.resoIdx, kResoNames.data(), (int)kResoNames.size());

    formLabel(ch, "Fullscreen");   // 布尔字段：标签在左列、框在控件列
    ImGui::Checkbox("##fullscreen", &st.fullscreen);

#ifdef _WIN32
    // 独占全屏是 Win32 的概念（acquireMonitor）；别的平台没有对应物，所以不画。
    // 呈现路径更短（少一次全帧 blt，见 docs/cli.md"全屏与录屏"），代价是 WGC/OBS 抓不到 ——
    // 默认不勾。它也决定 Alt+Enter 用哪种模式，所以不跟 Fullscreen 复选框联动置灰。
    formLabel(ch, "Exclusive mode");
    ImGui::Checkbox("##exclusive", &st.exclusive);
#endif

    formLabel(ch, "MSAA");         // Off/2x/4x/8x
    ImGui::SetNextItemWidth(formCtrlW(ch));
    ImGui::Combo("##msaa", &st.msaaIdx, kMsaaNames.data(), (int)kMsaaNames.size());

    formLabel(ch, "Cull block");   // 粗块 AABB 索引：只影响成本/内存，不影响出图
    ImGui::SetNextItemWidth(formCtrlW(ch));
    ImGui::Combo("##cullblock", &st.cullBlockIdx, kCullBlockNames.data(),
                 (int)kCullBlockNames.size());
    // 这里原来挂过一个悬停说明（"每块多少层 / 4k≈21MB / 256≈320MB / 不改变出图"）——
    // 用户指出"其他地方都没有"，向导里只有这一个控件有 tooltip，很突兀 → 去掉 ✗。
    // 这份权衡有正式去处：`docs/rendering.md`「可见性/剔除缓存」+ `tests/cull_cost_probe.cpp`。

    formStyleEnd();

    bool back = false;
    // Back 时 drawNavButton 返回 false（只有 `back` 出参知道按下去了）→ 必须 `|| back`
    if (drawNavButton(ch, false, true, /*showBack=*/true, &back) || back) {
        st.page = back ? prevPage(st.page) : nextPage(st.page);   // Graphics → Trail（别再写死 ✗）
    }
}

} // namespace wizard
