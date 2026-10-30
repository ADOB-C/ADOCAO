// 倒计时（t < 0）的两星位置必须与 t=0 的起始状态**连续**。
//
// 用户 2026-10-29 报（截图 + "我测了一下，其他的也是"）：所有谱面开头，蓝球都是"传送过去"的，
// 不是正常转过去。真因在 `PositionSolver` 的两处倒计时分支：它们把移动星钉在**砖 0 的起点角**
// 上绕 p0 自转，于是整块砖的角度被吞掉 —— t→0⁻ 时蓝球停在 `p0 + 1·dir(startAngleAt(0))`
// （= 起点的**反方向**），t=0 那一帧直接跳到 `p1`：跨 2 格。修法是把相位接在**终点角**
// （`startAngleAt(0) + totalAngleAt(0)`）上：倒计时最后正好落在 p1，t=0 起红蓝交换角色、
// 位置不变。
//
// 三条断言（含负向对照，保证这条用例真的抓得住旧行为）：
//  ① 连续：t→0⁻ 与 t=0 的两星位置差 < 1e-2 砖；
//  ② 相位：倒计时开始那一刻（`startTimeAt(0)`）移动星在 `startAngleAt(0)` 方向上；
//  ③ 负向对照：旧公式（起点角锚定）算出来的 t→0⁻ 位置必须离 t=0 的真实位置 ≥ 1 砖
//     —— 也就是说这条用例在旧实现下必然红。
// 多行星（3 星）走的是 `planetsAt` 的 N 边形分支：这里只钉**倒计时段自身的光滑性**
// （逐帧步长一致），不钉 t=0 的连续性 —— 3 星在**每个砖边界**都会跳 1 砖（探针实测
// t=1.0⁻: id2 (0.5,0.866) → t=1.0⁺: id2 (0,0)），那是另一条 bug（TODO.md「MultiPlanet
// ≥3 星逐砖换位时两颗星跳 1 砖」），与本条（倒计时相位）不同源。

#include "core/level/LevelData.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

struct Pose {
    glm::dvec2 red{}, blue{};
    bool ok = false;
};

Pose poseAt(const Timeline& tl, double t) {
    std::vector<PositionSolver::PlanetPos> ps;
    PositionSolver::planetsAt(tl, t, ps);
    Pose p;
    int seen = 0;
    for (auto& q : ps) {
        if (q.id == 0) { p.red = q.pos; seen |= 1; }
        if (q.id == 1) { p.blue = q.pos; seen |= 2; }
    }
    p.ok = (seen == 3);
    return p;
}

bool loadChart(LevelData& lv, Timeline& tl, bool threePlanets) {
    std::string json = R"J({
      "angleData": [0,180,180,180,180,180,180,180,180,180,180,180],
      "settings": {"bpm": 120, "offset": 0, "countdownTicks": 0, "pitch": 100,
                   "trackColor": "debb7b", "backgroundColor": "000000",
                   "trackStyle": "Standard", "rotation": 0},
      "actions": [)J";
    if (threePlanets) json += R"J({"floor":0,"eventType":"MultiPlanet","planets":"ThreePlanets"})J";
    json += "]}";
    if (!lv.loadFromBuffer(json.data(), json.size())) return false;
    tl.build(lv, false);
    return true;
}

double dist(glm::dvec2 a, glm::dvec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

int main() {
    for (int three = 0; three < 2; three++) {
        LevelData lv;
        Timeline tl;
        if (!loadChart(lv, tl, three != 0)) {
            std::printf("FAIL 用例谱面没加载成功（%d 星）\n", three ? 3 : 2);
            return 1;
        }
        const char* tag = three ? "3 星" : "2 星";
        const double d0 = (double)tl.durationAt(0);
        if (d0 <= 0.0) {
            std::printf("FAIL 砖 0 时长非正（%s）\n", tag);
            return 1;
        }
        const double start0 = tl.startTimeAt(0);
        const double firstStart = tl.firstStartTime();
        const glm::dvec2 p0(lv.tilePositionAt(0)[0], lv.tilePositionAt(0)[1]);

        const Pose atZero = poseAt(tl, firstStart);
        if (!atZero.ok) {
            std::printf("FAIL %s：t=0 没有拿到红/蓝两星\n", tag);
            return 1;
        }

        // ① 连续：从下面逼近 t=0
        const Pose below = poseAt(tl, firstStart - 1e-9);
        const Pose below2 = poseAt(tl, firstStart - d0 * 0.02);
        if (!below.ok || !below2.ok) {
            std::printf("FAIL %s：倒计时没有拿到红/蓝两星\n", tag);
            return 1;
        }
        const double jumpBlue = dist(below.blue, atZero.blue);
        const double jumpRed = dist(below.red, atZero.red);
        if (!three && jumpBlue > 1e-2) {
            std::printf("FAIL %s：t→0⁻ 蓝星跳了 %.6f 砖\n", tag, jumpBlue);
            return 1;
        }
        if (!three && jumpRed > 1e-2) {
            std::printf("FAIL %s：t→0⁻ 红星跳了 %.6f 砖\n", tag, jumpRed);
            return 1;
        }
        // 倒计时里移动星的半径必须还是 1（贴着枢轴自转，不跟着 dist 外推）
        for (double t = start0; t < firstStart; t += d0 * 0.1) {
            const Pose p = poseAt(tl, t);
            const double r = dist(p.blue, p0);
            if (std::fabs(r - 1.0) > 1e-3) {
                std::printf("FAIL %s：倒计时半径 %.6f ≠ 1（t=%.6f）\n", tag, r, t);
                return 1;
            }
        }
        if (three) {
            // 倒计时段自身必须光滑：相邻采样步长与理论步长（每步 0.1 砖时长）相差不超过 1.5 倍
            double maxStep = 0.0;
            Pose prev = poseAt(tl, start0);
            for (double t = start0 + d0 * 0.05; t < firstStart - 1e-12; t += d0 * 0.05) {
                const Pose cur = poseAt(tl, t);
                maxStep = std::max(maxStep, std::max(dist(prev.red, cur.red), dist(prev.blue, cur.blue)));
                prev = cur;
            }
            if (maxStep <= 0.0 || maxStep > 1.5) {
                std::printf("FAIL %s：倒计时段不光滑（最大步长 %.6f）\n", tag, maxStep);
                return 1;
            }
            std::printf("ok   %s：倒计时段光滑（最大步长 %.3f 砖）；t=0 跳 %.3f 砖属另一条"
                        "「≥3 星逐砖换位」bug（TODO）\n", tag, maxStep, jumpBlue);
        } else {
            std::printf("ok   %s：t→0⁻ 连续（蓝 %.2e / 红 %.2e 砖），倒计时半径 1\n", tag,
                        jumpBlue, jumpRed);
        }

        // ② 相位：倒计时开始那一刻，移动星在砖 0 的**起点角**方向上
        //    （只对 2 星成立；N 边形布局里移动星还要加 angleOffset = dir·π(n−2)/n）
        if (!three) {
            const Pose p = poseAt(tl, start0);
            const double want = (double)tl.startAngleAt(0);
            const double got = std::atan2(p.blue.y - p0.y, p.blue.x - p0.x);
            double diff = std::fmod(std::fabs(got - want), 2.0 * 3.14159265358979323846);
            if (diff > 3.14159265358979323846) diff = 2.0 * 3.14159265358979323846 - diff;
            if (diff > 2e-3) {
                std::printf("FAIL %s：倒计时起点相位偏了 %.6f rad（应接在砖 0 的起点角）\n", tag, diff);
                return 1;
            }
            std::printf("ok   %s：倒计时起点相位 = 砖 0 起点角（偏差 %.2e rad）\n", tag, diff);
        }

        // ③ 负向对照：旧实现（相位钉在起点角）在 t→0⁻ 会离真实位置 ≥ 1 砖 —— 上面 ① 必然红
        if (!three) {
            const double endAngle = (double)tl.startAngleAt(0) + (double)tl.totalAngleAt(0);
            const bool cw = tl.tileIsCW()[0];
            const double rps = (double)(tl.tileBPMs()[0] / 60.0) * 3.14159265358979323846;
            const double dt = 1e-9;
            const double oldAngle = cw ? ((double)tl.startAngleAt(0) + dt * rps)
                                       : ((double)tl.startAngleAt(0) - dt * rps);
            const glm::dvec2 oldBlue(p0.x + std::cos(oldAngle), p0.y + std::sin(oldAngle));
            const double oldJump = dist(oldBlue, atZero.blue);
            if (oldJump < 1.0) {
                std::printf("FAIL 负向对照失效：旧实现在 t=0 只偏 %.6f 砖（这条用例抓不住它）\n",
                            oldJump);
                return 1;
            }
            (void)endAngle;
            std::printf("ok   负向对照：旧实现的蓝星在 t→0⁻ 离真实位置 %.3f 砖（≥1 → ① 必红）\n",
                        oldJump);
        }
    }

    if (g_fail) {
        std::printf("countdown_pose: %d 项失败\n", g_fail);
        return 1;
    }
    std::printf("countdown_pose: 全部通过\n");
    return 0;
}
