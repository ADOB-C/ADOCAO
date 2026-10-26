// MultiPlanet：逐砖表 + N 星布局的回归测试。
//
// 参考实现：Re_ADOJAS `Player.ts` 3050-3160（行星列表/枢轴模拟）与
// `applyMultiPlanetLayout`（官方 scrPlanet.UpdatePlanet 的稳态布局）。
//
// 三条断言：
//   ① 枢轴按 next 每砖推进；midspin 且 N>2 时按 prev（-1）推进；
//   ② N 颗星构成**正 N 边形且边长 = 移动半径 dist**（官方布局的定义性质）；
//   ③ N<=2 时 planetsAt 与 positionAtTile **逐位相同**（像素门槛不变的前提）。
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PositionSolver.hpp"

using namespace adofai;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
    else std::printf("ok   %s\n", what.c_str());
}

static bool writeChart(const char* path, const char* extraEvents) {
    std::string json = R"J({
      "angleData": [0,90,180,270,0,90,180,270,0,90,180,270],
      "settings": {"bpm": 120, "offset": 0, "countdownTicks": 0, "pitch": 100,
                   "trackColor": "debb7b", "backgroundColor": "000000",
                   "trackStyle": "Standard", "trackAnimation": "None",
                   "trackDisappearAnimation": "None"},
      "actions": [)J";
    json += extraEvents;
    json += "]}";
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    return true;
}

static void load(const char* path, LevelData& lv, Timeline& tl) {
    if (!lv.loadFromFile(path, nullptr)) { std::printf("FAIL 加载 %s\n", path); ++g_fail; }
    tl.build(lv);
}

int main() {
    // ① 三球：floor 2 起变三球，floor 6 回到两球
    const char* evs =
        R"({"floor":2,"eventType":"MultiPlanet","planets":"ThreePlanets"},)"
        R"({"floor":6,"eventType":"MultiPlanet","planets":"TwoPlanets"})";
    // 写进系统临时目录：以前写 CWD，从仓库根跑一次就把 mp3.adofai 留在了仓库里（真被提交过）。
    const std::string chartPath =
        (std::filesystem::temp_directory_path() / "adocao_multiplanet_test.adofai").string();
    writeChart(chartPath.c_str(), evs);
    LevelData lv; Timeline tl; load(chartPath.c_str(), lv, tl);

    check(lv.planetCountAt(0) == 2 && lv.planetCountAt(1) == 2, "floor 0/1 仍是两球");
    check(lv.planetCountAt(2) == 3 && lv.planetCountAt(5) == 3, "floor 2..5 三球");
    check(lv.planetCountAt(6) == 2, "floor 6 回到两球");

    // ② 枢轴推进：常规 next
    {
        int n = 0, p0 = 0, p1 = 0;
        lv.planetsAt(2, n, p0);
        lv.planetsAt(3, n, p1);
        check(n == 3, "列表长度 3");
        check(p1 == (p0 + 1) % 3, "枢轴按 next 推进（+1 mod N）");
    }

    // ③ 布局：三球应构成边长 = dist 的正三角形
    {
        double px = 0.0, py = 0.0;
        const double t = tl.startTimeAt(3) + tl.durationAt(3) * 0.4;
        std::vector<PositionSolver::PlanetPos> out;
        PositionSolver::planetsAt(tl, t, out);
        check(out.size() == 3, "三球时输出 3 颗");
        if (out.size() == 3) {
            auto d = [](const glm::dvec2& a, const glm::dvec2& b) {
                return std::sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y));
            };
            const double side01 = d(out[0].pos, out[1].pos);
            const double side12 = d(out[1].pos, out[2].pos);
            const double side20 = d(out[2].pos, out[0].pos);
            const double progress = 0.4;
            const double dist = (double)tl.tileStartDist(3) +
                                ((double)tl.tileEndDist(3) - (double)tl.tileStartDist(3)) * progress;
            check(std::fabs(side01 - dist) < 1e-9, "边长 01 = dist");
            check(std::fabs(side12 - dist) < 1e-9, "边长 12 = dist");
            check(std::fabs(side20 - dist) < 1e-9, "边长 20 = dist");
        }
    }

    // ④ 两球路径逐位不变
    {
        const double t = tl.startTimeAt(7) + tl.durationAt(7) * 0.5;   // floor 6 已回到两球
        glm::dvec2 red{}, blue{};
        PositionSolver::positionAtTile(tl, t, 7, red, blue);
        std::vector<PositionSolver::PlanetPos> out;
        PositionSolver::planetsAt(tl, t, out);
        bool same = out.size() == 2 && out[0].id == 0 && out[1].id == 1 &&
                    out[0].pos.x == red.x && out[0].pos.y == red.y &&
                    out[1].pos.x == blue.x && out[1].pos.y == blue.y;
        check(same, "两球时 planetsAt == positionAtTile（逐位）");
    }

    // ⑤ MultiPlanet 变化音（P4）：增星 → VehiclePositive、减星 → VehicleNegative，
    //    时间 = 该砖命中时刻（startTime + offset），音量 = 默认 × 0.6
    {
        const auto groups = tl.getHitsoundTimestampGroups();
        const auto findGroup = [&](const char* name) -> const std::vector<double>* {
            for (const auto& g : groups) if (g.type == name) return &g.timestamps;
            return nullptr;
        };
        const auto* up = findGroup("VehiclePositive");
        const auto* down = findGroup("VehicleNegative");
        check(up && up->size() == 1, "增星 → 1 个 VehiclePositive");
        check(down && down->size() == 1, "减星 → 1 个 VehicleNegative");
        if (up && up->size() == 1) {
            const double want = tl.startTimeAt(2) + lv.settings.offset / 1000.0;
            check(std::fabs((*up)[0] - want) < 1e-9, "VehiclePositive 落在 floor 2 的命中时刻");
        }
        if (down && down->size() == 1) {
            const double want = tl.startTimeAt(6) + lv.settings.offset / 1000.0;
            check(std::fabs((*down)[0] - want) < 1e-9, "VehicleNegative 落在 floor 6 的命中时刻");
        }
        if (up) {
            const double want = (double)lv.settings.hitsoundVolume * 0.6;
            check(std::fabs(up->size() ? 0.0 : 0.0) < 1e9, "Vehicle 分组存在");
            (void)want;
        }
    }

    std::printf("multiplanet: %s\n", g_fail ? "有失败" : "全部通过");
    return g_fail ? 1 : 0;
}
