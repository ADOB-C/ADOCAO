// 两套 zoom 口径的换算（`render/Camera`）：
//
//   * 本类 `setZoom(z)`：放大倍数，z 越大越近（100 = 可见高度 12 砖）。
//   * 谱面（ADOFAI/Re_ADOJAS）的 zoom：越大看得越远，100 = 可见高度 5.47 砖。
//
// 谱面值必须走 `setAdoZoom()`。方向搞反的后果不是"略大略小"，而是**反向缩放**：
// settings.zoom=200 会变成"放大 2 倍"（视野只剩 6 砖），用户在 200 zoom 的谱上看到的就是
// "视野太小"。这个用例把方向和换算都钉死。

#include "render/Camera.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

int g_fail = 0;

void expectNear(const char* what, double got, double want, double tol) {
    if (!(std::fabs(got - want) <= tol)) {
        std::fprintf(stderr, "FAIL %s: got=%.6f want=%.6f (tol %.1e)\n", what, got, want, tol);
        ++g_fail;
    }
}

double visibleHeight(const adofai::Camera& c) {
    double l = 0, r = 0, b = 0, t = 0;
    c.frustumBounds(l, r, b, t);
    return t - b;
}

}  // namespace

int main() {
    using adofai::Camera;

    // 1) 本类口径不变（滚轮 / --capture-zoom 靠它，像素基线也靠它）。
    {
        Camera c;
        c.setZoom(100.0f);
        expectNear("setZoom(100) 可见高 12", visibleHeight(c), 12.0, 1e-4);
        c.setZoom(200.0f);
        expectNear("setZoom(200) 可见高 6（越大越近）", visibleHeight(c), 6.0, 1e-4);
        c.setZoom(50.0f);
        expectNear("setZoom(50) 可见高 24", visibleHeight(c), 24.0, 1e-4);
    }

    // 2) ADOFAI 口径：100 = 5.47 砖，**越大看得越远**。
    {
        Camera c;
        c.setAdoZoom(100.0f);
        expectNear("setAdoZoom(100) 可见高 5.47", visibleHeight(c), 5.47, 1e-3);
        c.setAdoZoom(200.0f);
        expectNear("setAdoZoom(200) 可见高 10.94（越大越远）", visibleHeight(c), 10.94, 1e-3);
        c.setAdoZoom(50.0f);
        expectNear("setAdoZoom(50) 可见高 2.735", visibleHeight(c), 2.735, 1e-3);
        c.setAdoZoom(500.0f);
        expectNear("setAdoZoom(500) 可见高 27.35", visibleHeight(c), 27.35, 1e-3);
    }

    // 3) 单调：adoZoom 增大 → 可见高度增大（和本类的方向相反）。
    {
        Camera c;
        double prev = 0.0;
        for (float z : {20.0f, 50.0f, 100.0f, 200.0f, 400.0f}) {
            c.setAdoZoom(z);
            const double h = visibleHeight(c);
            if (!(h > prev)) {
                std::fprintf(stderr, "FAIL 单调性：adoZoom=%.1f 时可见高 %.6f 没有增大（上一个 %.6f）\n",
                             z, h, prev);
                ++g_fail;
            }
            prev = h;
        }
    }

    // 4) 两套口径的换算一致：setAdoZoom(z) == setZoom(21937.8427…/z)。
    {
        Camera a, b;
        for (float z : {37.0f, 100.0f, 137.0f, 333.0f}) {
            a.setAdoZoom(z);
            b.setZoom((float)(120000.0 / 5.47 / z));
            expectNear("换算一致", visibleHeight(a), visibleHeight(b), 1e-3);
        }
    }

    // 5) 非法值兜底：0 / 负数 / 极大 → 有限、非零、不翻负。
    {
        Camera c;
        c.setAdoZoom(0.0f);
        expectNear("adoZoom=0 → 按 100", visibleHeight(c), 5.47, 1e-3);
        c.setAdoZoom(-50.0f);
        expectNear("adoZoom<0 → 按 100", visibleHeight(c), 5.47, 1e-3);
        c.setAdoZoom(1e9f);
        const double h = visibleHeight(c);
        if (!(h > 0.0) || !std::isfinite(h)) {
            std::fprintf(stderr, "FAIL adoZoom=1e9 的可见高不是有限正数：%f\n", h);
            ++g_fail;
        }
    }

    if (g_fail) { std::fprintf(stderr, "camera_zoom: %d 处失败\n", g_fail); return 1; }
    std::printf("camera_zoom: 两套 zoom 口径（本类 12 砖@100 / ADOFAI 5.47 砖@100）方向与换算都对\n");
    return 0;
}
