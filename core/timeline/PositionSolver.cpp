#include "core/timeline/PositionSolver.hpp"
#include <cstdlib>   // getenv（诊断）
#include "core/timeline/Timeline.hpp"
#include "core/timeline/TileXform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <chrono>
#include "core/util/CountLimits.hpp"
#include "core/util/Logger.hpp"
#include "core/util/Env.hpp"


namespace adofai {

static constexpr double kPi = 3.14159265358979323846;

// 行星枢轴：`stickToFloors` 只管 **PositionTrack** 的静态偏移（`planetPivotAt`）。
//
// **MoveTrack 动画暂不跟**（2026-10-24 回退）：让行星跟着逐砖动画走之后，动画的位移
// 会在相邻采样点之间造成几十~上百单位的大跳，拖尾直接拉成贯穿全屏的直线
// （用户："你这个直线也是才出现的"）。参考实现是靠 `MoveTrackManager` 里
// "按历史时刻重建位置"给拖尾专门兜住的 —— 那套还没移植，所以先保持"不跟动画"，
// 等把动画布局本身对齐 + 拖尾能重建历史位置之后再打开。
static void pivotAt(const LevelData& L, int64_t idx, double t, double& x, double& y) {
    (void)t;
    L.planetPivotAt(idx, x, y);
}

// 行星绕行半径（砖单位）：ScaleRadius 的继承值在砖内从 `r(i)` 线性插到 `r(i+1)`
// （参考实现 `computePositionsAtTime`：`orbitR = r0 + (rTail − r0)·hp`，rTail 取 i+1）。
// **与砖间实际距离无关** —— 用户 2026-10-30："只有在 ScaleRadius 的时候再改变半径"。
inline double orbitRadiusAt(const LevelData& L, int tileIdx, double progress) {
    const double r0 = (double)L.radiusScaleAt(tileIdx);
    const double r1 = (double)L.radiusScaleAt((int64_t)tileIdx + 1);
    return r0 + (r1 - r0) * progress;
}

// 行星枢轴在 t 时刻的**活**位置：静态枢轴（已含 PositionTrack 偏移与 stickToFloors 语义）
// 再叠轨道动画的位移。
//
// 为什么只叠位移、不叠旋转/缩放：着色器（`assets/shaders/tile.vert`）是绕**砖的起点**
// （= 这个枢轴）转/缩的，枢轴本身不动；参考实现也只取 `tileMesh.position`
// （= 砖起点 + positionOffset）。所以旋转/缩放不改变行星的轨道中心 —— 这是权威口径。
//
// `stick=false` 的砖行星不跟它走（PositionTrack 那套已经由 `planetPivotAt` 减回原始位；
// 动画位移同样不能加），判据与参考实现的 `useStickToFloor` 一致。
//
// `moved` 用"或"累积：调用方要的是"这三块砖里**有没有**活位移"。
static void livePivot(const LevelData& L, const TileAnimation* anim, int64_t idx, double t,
                      double& x, double& y) {
    L.planetPivotAt(idx, x, y);
    if (!anim || idx < 0 || idx >= L.tileCount()) return;
    if (!L.stickyAt(idx)) return;
    const TileXform xf = tileXformAt(L, anim, idx, t);
    x += xf.moveX;
    y += xf.moveY;
}


void PositionSolver::positionAt(const Timeline& timeline, double t, glm::dvec2& redOut,
                                glm::dvec2& blueOut, const TileAnimation* anim) {
    const LevelData& L = *timeline.level();   // ③ 切片 4：走访问器，不直接碰 tiles[]
    const int64_t nFull = L.tileCount();   // #0：int64 算，收窄前显式判
    if (!fitsInt32(nFull)) {
        LOG_E("PositionSolver 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32", (long long)nFull);
        return;
    }
    const int n = (int)nFull;
    if (n < 2) return;
    int tileIdx = timeline.findTileIndex(t);

    // 倒计时（t < 0）：蓝星绕首砖的枢轴转 —— **相位接在砖 0 的"终点角"上**。
    // 这样 t→0⁻ 时蓝星正好落在 p1（= 砖 1 的枢轴位置），而 t=0 起走砖 1 的行程
    // （红蓝交换角色、位置不变）→ 两星位置**连续**。
    // 旧写法把它钉在起点角（`rotation+180` = `startAngleAt(0)`）上：整块砖的角度被吞掉，
    // t=0 那一帧蓝星从枢轴对侧横跨 2 格跳到 p1（用户 2026-10-29："开头蓝球是传送过去的，
    // 不是正常转过去"，任何谱都能复现）。半径仍是 1（倒计时两星贴身自转，不跟着 dist 外推）。
    if (t < timeline.firstStartTime()) {
        const auto& p0 = L.tilePositionAt(0);
        double bpm = timeline.tileBPMs()[0];
        bool cw = timeline.tileIsCW()[0];
        // 终点角 = 起点角 + 本砖总角（弧度）；倒计时里随着 t 前进往回退，dt=0 时正好是终点角
        double endAngle = (double)timeline.startAngleAt(0) + (double)timeline.totalAngleAt(0);
        double dt = timeline.firstStartTime() - t;
        double rps = (bpm / 60.0) * 3.14159265358979;
        double angle = cw ? (endAngle + dt * rps) : (endAngle - dt * rps);
        double dist = 1.0;
        glm::dvec2 mv(p0[0] + std::cos(angle) * dist, p0[1] + std::sin(angle) * dist);
        glm::dvec2 pv(p0[0], p0[1]);
        redOut = pv; blueOut = mv;
        return;
    }

    if (tileIdx >= n - 1) {
        // tileCount 含末尾合成砖（index n-1）；真实最后一格是 n-2。
        // 旧代码把结尾环绕中心放在合成砖（最后一格的**终点角**）上，导致结尾整体偏一格、
        // 最后一格看起来"没画出来"（2026-10-18 用户实测：任何谱结尾都偏）。
        int lastIdx = n - 2;
        if (lastIdx < 0) lastIdx = 0;
        double lx = 0.0, ly = 0.0;
        pivotAt(L, lastIdx, t, lx, ly);
        const double pivotPos[2] = {lx, ly};
        double startAngle = 0.0;
        if (lastIdx > 0) {
            const auto& prevPos = L.tilePositionAt(lastIdx - 1);
            startAngle = std::atan2(prevPos[1]-pivotPos[1], prevPos[0]-pivotPos[0]);
        }
        double extraTime = t - timeline.startTimeAt(lastIdx);
        double rps = (double)(timeline.tileBPMs()[lastIdx]/60.0) * 3.14159265358979;
        double currentAngle = timeline.tileIsCW()[lastIdx] ? (startAngle-extraTime*rps) : (startAngle+extraTime*rps);
        glm::dvec2 mv(pivotPos[0]+std::cos(currentAngle), pivotPos[1]+std::sin(currentAngle));
        if (lastIdx%2==0) { redOut=glm::dvec2(pivotPos[0],pivotPos[1]); blueOut=mv; }
        else              { blueOut=glm::dvec2(pivotPos[0],pivotPos[1]); redOut=mv; }
        return;
    }

    bool isRed = (tileIdx%2==0);
    // 粘性轨道 + 轨道动画：**枢轴跟着砖走**（平移），**半径只由 ScaleRadius 决定** ——
    // 用户在 2026-10-30 明确："只有在 ScaleRadius 的时候再改变半径"。参考实现的纯时间函数
    // `computePositionsAtTime` 同口径：`orbitR = tileRadiusScale(i) → (i+1)` 段内插值，
    // 并且**显式丢弃** `startDist/endDist`。
    double pvx = 0.0, pvy = 0.0;
    livePivot(L, anim, tileIdx, t, pvx, pvy);
    const double pivotPos[2] = {pvx, pvy};
    double startTime = timeline.startTimeAt(tileIdx);
    double duration = timeline.durationAt(tileIdx);
    double progress = (duration>0.0)?(t-startTime)/duration:1.0;
    if (progress<0) progress=0; if (progress>1) progress=1;
    double angle = (double)timeline.startAngleAt(tileIdx)+(double)timeline.totalAngleAt(tileIdx)*progress;
    double dist = orbitRadiusAt(L, tileIdx, progress);
    // **长按**：权威 `Player.ts:7213` —— 圆心从砖心**线性平移**到 `tail位置 − rTail·dir(exitDir)`，
    // 其中 `exitDir = startAngle + totalAngle` 是**固定**的出口角（不是当前角）。
    // 行星/拖尾/相机都跟着这个滑动圆心走（相机跟行星，所以改这里一处就够）。
    double cx = pivotPos[0], cy = pivotPos[1];
    if (L.holdLengthAt(tileIdx) >= 0 && tileIdx + 1 < L.tileCount()) {
        const double exitDir = (double)timeline.startAngleAt(tileIdx) + (double)timeline.totalAngleAt(tileIdx);
        const auto tail = L.tilePositionAt(tileIdx + 1);
        const double rTail = (double)L.radiusScaleAt(tileIdx + 1);
        const double endCX = (double)tail[0] - std::cos(exitDir) * rTail;
        const double endCY = (double)tail[1] - std::sin(exitDir) * rTail;
        cx += (endCX - cx) * progress;
        cy += (endCY - cy) * progress;
    }
    glm::dvec2 pv(cx, cy);
    glm::dvec2 mv(cx+std::cos(angle)*dist, cy+std::sin(angle)*dist);
    if (isRed) { redOut=pv; blueOut=mv; }
    else       { blueOut=pv; redOut=mv; }
}

void PositionSolver::planetsAt(const Timeline& timeline, double t, std::vector<PlanetPos>& out,
                               const TileAnimation* anim) {
    out.clear();
    const LevelData& L = *timeline.level();
    const int64_t nFull = L.tileCount();
    if (nFull < 2) return;
    int tileIdx = (int)timeline.findTileIndex(t);
    if (tileIdx < 0) tileIdx = 0;
    const int count = L.planetCountAt(tileIdx);
    if (count <= 2) {
        glm::dvec2 red{}, blue{};
        positionAtTile(timeline, t, tileIdx, red, blue, anim);
        out.push_back({0, red});
        out.push_back({1, blue});
        return;
    }

    int n = count, pivotPos = 0;
    const uint8_t* ids = L.planetsAt(tileIdx, n, pivotPos);
    if (n <= 2) {   // 列表退化（理论上不会），回两星口径
        glm::dvec2 red{}, blue{};
        positionAtTile(timeline, t, tileIdx, red, blue, anim);
        out.push_back({0, red});
        out.push_back({1, blue});
        return;
    }

    // 活几何（与两星路径同源）：枢轴跟着砖平移；半径只由 ScaleRadius 决定。
    double px = 0.0, py = 0.0;
    livePivot(L, anim, tileIdx, t, px, py);
    const double startTime = timeline.startTimeAt(tileIdx);
    const double duration = timeline.durationAt(tileIdx);
    double progress = (duration > 1e-9) ? (t - startTime) / duration : 1.0;
    if (progress < 0) progress = 0;
    if (progress > 1) progress = 1;
    const double angle = (double)timeline.startAngleAt(tileIdx) +
                         (double)timeline.totalAngleAt(tileIdx) * progress;
    const double dist = orbitRadiusAt(L, tileIdx, progress);
    const double dirSign = timeline.tileIsCW()[tileIdx] ? -1.0 : 1.0;
    const double inverse = kPi * (n - 2) / n;

    // 移动星的起始角偏移（官方 scrPlanet.Update_RefreshAngles 1625-1631，含 midspin 修正）
    double angleOffset = dirSign * inverse;
    if (tileIdx > 0 && L.angleAt(tileIdx - 1) == 999.0) {
        int prevN = 2, prevPivot = 0;
        L.planetsAt(tileIdx - 1, prevN, prevPivot);
        if (prevN > 2) {
            const double prevDir = timeline.tileIsCW()[tileIdx - 1] ? -1.0 : 1.0;
            const double prevInverse = kPi * (prevN - 2) / prevN;
            angleOffset -= prevDir * prevInverse;
        }
    }
    const double num = angle + angleOffset;
    out.push_back({ids[pivotPos], {px, py}});                                  // 枢轴星
    const int moverIdx = (pivotPos + 1) % n;
    out.push_back({ids[moverIdx], {px + std::cos(num) * dist, py + std::sin(num) * dist}});

    const double R = dist / (2.0 * std::sin(kPi / n));
    const double num4 = num - dirSign * inverse / 2.0;
    const double cx = px + std::cos(num4) * R;
    const double cy = py + std::sin(num4) * R;
    for (int j = 1; j <= n - 2; ++j) {
        const int id = ids[(pivotPos + 1 + j) % n];
        const double a = num4 + dirSign * 2.0 * kPi * (-(double)(j + 1) - n / 2.0) / n;
        out.push_back({(uint8_t)id, {cx + std::cos(a) * R, cy + std::sin(a) * R}});
    }
}

void PositionSolver::positionAtTile(const Timeline& timeline, double t, int tileIdx,
                                    glm::dvec2& redOut, glm::dvec2& blueOut,
                                    const TileAnimation* anim) {
    const LevelData& L = *timeline.level();   // ③ 切片 4：走访问器，不直接碰 tiles[]
    const int64_t nFull = L.tileCount();   // #0：int64 算，收窄前显式判
    if (!fitsInt32(nFull)) {
        LOG_E("PositionSolver 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32", (long long)nFull);
        return;
    }
    const int n = (int)nFull;
    if (n < 2) return;
    if (t < timeline.firstStartTime()) {
        // 与 positionAt 的倒计时分支**逐字同源**（见那里的说明：相位必须接在砖 0 的终点角上，
        // 否则 t=0 会跳 2 格）。
        const auto& p0 = L.tilePositionAt(0);
        double bpm = timeline.tileBPMs()[0]; bool cw = timeline.tileIsCW()[0];
        double endAngle = (double)timeline.startAngleAt(0) + (double)timeline.totalAngleAt(0);
        double dts = timeline.firstStartTime() - t;
        double rps = (bpm / 60.0) * 3.14159265358979;
        double angle = cw ? (endAngle + dts * rps) : (endAngle - dts * rps);
        glm::dvec2 mv(p0[0] + std::cos(angle), p0[1] + std::sin(angle));
        glm::dvec2 pv(p0[0], p0[1]);
        redOut = pv; blueOut = mv;
        return;
    }
    if (tileIdx >= n - 1) {
        // tileCount 含末尾合成砖（index n-1）；真实最后一格是 n-2。
        // 旧代码把结尾环绕中心放在合成砖（最后一格的**终点角**）上，导致结尾整体偏一格、
        // 最后一格看起来"没画出来"（2026-10-18 用户实测：任何谱结尾都偏）。
        int lastIdx = n - 2;
        if (lastIdx < 0) lastIdx = 0;
        double lx = 0.0, ly = 0.0;
        pivotAt(L, lastIdx, t, lx, ly);
        const double pivotPos[2] = {lx, ly};
        double startAngle = 0.0;
        if (lastIdx > 0) {
            const auto& prevPos = L.tilePositionAt(lastIdx - 1);
            startAngle = std::atan2(prevPos[1]-pivotPos[1], prevPos[0]-pivotPos[0]);
        }
        double extraTime = t - timeline.startTimeAt(lastIdx);
        double rps = (double)(timeline.tileBPMs()[lastIdx]/60.0) * 3.14159265358979;
        double currentAngle = timeline.tileIsCW()[lastIdx] ? (startAngle-extraTime*rps) : (startAngle+extraTime*rps);
        glm::dvec2 mv(pivotPos[0]+std::cos(currentAngle), pivotPos[1]+std::sin(currentAngle));
        if (lastIdx%2==0) { redOut=glm::dvec2(pivotPos[0],pivotPos[1]); blueOut=mv; }
        else              { blueOut=glm::dvec2(pivotPos[0],pivotPos[1]); redOut=mv; }
        return;
    }
    bool isRed = (tileIdx%2==0);
    // 与 positionAt 主分支**同一套口径**（见那里的说明）：枢轴跟着砖平移，
    // 半径只由 ScaleRadius 决定（`r(i) → r(i+1)` 段内插值），不按活邻居重算。
    double pvx = 0.0, pvy = 0.0;
    livePivot(L, anim, tileIdx, t, pvx, pvy);
    const double pivotPos[2] = {pvx, pvy};
    double startTime = timeline.startTimeAt(tileIdx);
    double duration = timeline.durationAt(tileIdx);
    double progress = (duration>0.0)?(t-startTime)/duration:1.0;
    if (progress<0) progress=0; if (progress>1) progress=1;
    double angle = (double)timeline.startAngleAt(tileIdx)+(double)timeline.totalAngleAt(tileIdx)*progress;
    double dist = orbitRadiusAt(L, tileIdx, progress);
    glm::dvec2 pv(pivotPos[0],pivotPos[1]);
    glm::dvec2 mv(pivotPos[0]+std::cos(angle)*dist, pivotPos[1]+std::sin(angle)*dist);
    if (isRed) { redOut=pv; blueOut=mv; }
    else       { blueOut=pv; redOut=mv; }
}

double PositionSolver::tilePathSpeed(const Timeline& timeline, double t) {
    if (timeline.tileCount() == 0) return 0.0;
    const int idx = timeline.findTileIndex(t);   // 保证落在 [0, n-1]
    const double d = timeline.durationAt(idx);
    if (d <= 1e-9) return 0.0;

    // Track covered by one tile: at least the step to the next tile centre, plus
    // the arc swept by the tile's relative angle (a 180 deg / midspin tile makes
    // the planet travel a long arc while barely advancing). Sampling has to
    // follow that arc, otherwise slow sharp turns come out as chunky polylines.
    const double step = 1.0;
    const double rot  = std::abs((double)timeline.totalAngleAt(idx));
    const double radius = std::max((double)timeline.tileStartDist(idx),
                                   (double)timeline.tileEndDist(idx));
    const double arc = rot * std::max(0.5, radius);
    return std::max(step, arc) / d;
}

double PositionSolver::curvatureRadius(const Timeline& timeline, double t) {
    const int64_t n = timeline.tileCount();
    if (n <= 0) return 0.0;
    const int idx = (int)timeline.findTileIndex(t);
    // **R 必须和行星真正绕的半径同源**：`Timeline::tileStartDist/tileEndDist` 对所有非末层
    // 恒返回 1.0f（不是几何距离），所以以前这里永远当 R=1 —— 而半径早就是 ScaleRadius 给的
    // （`orbitRadiusAt`）。`scale: 200` 的谱上 R 会小 2 倍 → sMax 偏大 → 采样率欠。
    const LevelData& L = *timeline.level();
    const double start = timeline.startTimeAt(idx);
    const double dur = timeline.durationAt(idx);
    double progress = (dur > 0.0) ? (t - start) / dur : 1.0;
    progress = std::clamp(progress, 0.0, 1.0);
    return orbitRadiusAt(L, idx, progress);
}

PositionSolver::TrailWindow PositionSolver::trailWindow(const Timeline& timeline, double t,
                                                        const TrailSamplingConfig& cfg) {
    TrailWindow w;
    w.startTime = t;
    if (timeline.tileCount() == 0) { w.sampleRate = cfg.fixedRate; return w; }

    // 1) Window first: seconds (default) or a tile count back.
    double start = t - (double)cfg.duration;
    if (cfg.lengthInTiles) {
        int i = timeline.findTileIndex(t);   // 保证落在 [0, n-1]
        // Walk back by tile COUNT (not by accumulated seconds) and take a
        // fraction of the tile before that for a fractional request.
        const double back = std::max(0.0, (double)cfg.tiles);
        const int whole = (int)std::floor(back);
        const double frac = back - (double)whole;
        i = std::max(0, i - whole);
        start = timeline.startTimeAt(i);
        if (frac > 1e-6 && i > 0 && i - 1 < (int)timeline.tileCount())
            start -= frac * (double)timeline.durationAt(i - 1);
    }
    if (start > t) start = t;

    // 2) Rate. In tiles mode the rate is derived from the window so the point
    // count is exactly tiles x samplesPerTile whatever the BPM (a fixed Hz rate
    // is meaningless there: 8 tiles at BPM 1.5M is 0.3 ms, at 120 BPM it is 4 s).
    // rateMax is deliberately not applied — it exists to cap a Hz-mode rate, and
    // a large derived rate over a tiny window still yields few points.
    const double span = t - start;
    double rate;
    if (cfg.lengthInTiles) {
        rate = span > 1e-9
             ? (double)cfg.tiles * (double)cfg.samplesPerTile / span
             : (double)cfg.fixedRate;
    } else {
        rate = (double)cfg.fixedRate;
        if (cfg.speedAware)
            rate = std::max(rate, tilePathSpeed(timeline, t) * (double)cfg.samplesPerTile);
        // 只有**自适应 governor** 才夹 rateMin/rateMax（那是内部算法自己的边界）。
        // 用户显式给的固定率原样使用：rateMin 也不再抬（以前设 10/s 会被静默抬到 60/s）。
        // Sagitta 判据：每个样本在折线上允许偏离 `sagittaPx` 像素。
        // 圆弧上步长 s（砖）对应的偏离 ≈ s²/(8R) 砖 → 换成像素就是 s²·pxPerTile/(8R)。
        // 令其 ≤ sagittaPx ⇒ s ≤ sqrt(8·R·sagittaPx/pxPerTile)，rate = v/s。
        if (cfg.sagittaPx > 0.0f && cfg.pixelsPerTile > 0.0f) {
            const double R = curvatureRadius(timeline, t);
            if (R > 1e-6) {
                const double sMax = std::sqrt(8.0 * R * (double)cfg.sagittaPx / (double)cfg.pixelsPerTile);
                if (sMax > 1e-9) rate = std::max(rate, tilePathSpeed(timeline, t) / sMax);
            }
        }
        // **只有自适应 governor 才夹**。sagitta 是用户显式打开的像素级判据，
        // 它算多少就是多少 —— 以前把 `sagittaPx > 0` 也算进来，于是
        // `--trail-sagitta 0.5` 在 2.64e6 BPM 上被 rateMax=4000 夹到 **4000/s**：
        // 0.01 s 窗口只剩 41 个点（该砖 4,320 砖/秒 → 每砖 0.09 点），整条拖尾塌掉。
        // 实测（tile 141628）：sagitta 该给的量级是 3.5 万/s（`--trail-samples-per-tile 8`
        // 在同一帧给出 34,557/s），被夹成 4000 差 8.6 倍。
        if (cfg.adaptive)
            rate = std::clamp(rate, (double)cfg.rateMin, (double)cfg.rateMax);
    }

    // 3) 点数硬上限（环容量）：只在**真的**要截断时告警，不再静默改用户参数。
    if (span > 1e-9 && rate * span > (double)cfg.maxPoints) {
        rate = (double)cfg.maxPoints / span;
        static double lastWarn = -1e9;
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - lastWarn > 2.0) {
            lastWarn = now;
            LOG_W("trail: rate × span 超过点数上限 maxPoints=%.0f（span=%.3fs）→ 实际 %.1f/s；"
                  "再要更密请缩短拖尾窗口（--trail-duration / --trail-tiles），"
                  "采样率再高也不会超过这个点数（告警去重 2s）",
                  (double)cfg.maxPoints, span, rate);
        }
    }

    w.startTime = start;
    w.sampleRate = (float)rate;
    return w;
}

namespace {

// 每砖常量（`positionAtTile` 主分支里那些**只依赖 tileIdx** 的量）。
// 连续采样点绝大多数落在同一块砖上，把它提出循环 = 只少算、不改任何算术：
// 每个量都是 `(double)` 同一个访问器返回值，逐位相同。
struct TrailTileConsts {
    double pivotX = 0.0, pivotY = 0.0;
    double startTime = 0.0;
    double duration = 0.0;
    double startAngle = 0.0;
    double totalAngle = 0.0;
    double startDist = 0.0;
    double endDist = 0.0;
    // 长按砖：圆心从 pivot 线性平移到 pivot+slide（与 `positionAtTile` 同一算式）
    double slideX = 0.0, slideY = 0.0;
    bool hasSlide = false;
    double firstStart = 0.0;  // == timeline.firstStartTime()（首砖之前的特例判据）
    bool isRed = true;
    bool tileMain = false;    // false = 落在 `positionAtTile` 的两个特例（首砖之前 / 末尾合成砖）
};

// `startTime` 由调用方给（推进循环刚刚算过同一个值，别重复取一遍）。
inline void loadTrailTileConsts(const Timeline& timeline, int tileIdx, int n, double startTime,
                               TrailTileConsts& c) {
    const LevelData& L = *timeline.level();
    const auto pivot = L.tilePositionAt(tileIdx);
    c.pivotX = pivot[0];
    c.pivotY = pivot[1];
    c.startTime = startTime;
    c.startAngle = (double)timeline.startAngleAt(tileIdx);
    // totalAngle 只取一次：`durationAt` 内部会再算一遍 `totalAngleAt`，而它要读 angle 紧凑存储 +
    // `angleDirBefore` + `extraRotAt`（lower_bound）—— 极密谱里每次采样点各一遍，是主要成本。
    const float totalAngle = timeline.totalAngleAt(tileIdx);
    c.totalAngle = (double)totalAngle;
    c.duration = (double)timeline.durationFromTotalAngle(tileIdx, totalAngle);
    // 半径：起点 = ScaleRadius(i)，终点 = ScaleRadius(i+1)（砖内线性插值，与行星同源）
    c.startDist = (double)L.radiusScaleAt(tileIdx);
    c.endDist = (double)L.radiusScaleAt(tileIdx + 1);
    // **长按**：拖尾也必须用"滑动圆心"，否则行星在转圈、拖尾却沿轨道走 —— 看起来就是
    // 一条水平的直线（用户报的"拖尾是水平的"）。
    c.hasSlide = (L.holdLengthAt(tileIdx) >= 0) && (tileIdx + 1 < (int)L.tileCount());
    c.isRed = (tileIdx % 2 == 0);
    c.firstStart = timeline.firstStartTime();
    // 与 `positionAtTile` 的分支判据一致：n < 2 或首砖之前走特例；末尾合成砖（>= n-1）也走特例。
    c.tileMain = (n >= 2) && (tileIdx < n - 1);
}

// 把"只依赖 tileIdx"的常量换成 **t 时刻的活值**（粘性轨道 + 轨道动画）。
// 与 `positionAtTile` 的 `live` 分支同一套算式：枢轴 + 相邻砖按活位置，rotate/scale 不动枢轴。
// 返回 false = 这三块砖都没有位移，调用方继续用缓存好的静态常量（逐位不变、也不多花时间）。
inline void loadTrailTileConstsLive(const Timeline& timeline, const TileAnimation* anim,
                                   int tileIdx, double tt, TrailTileConsts& c) {
    void(0);
    const LevelData& L = *timeline.level();
    // **只挪枢轴**：半径/起始角一律用静态值（半径只由 ScaleRadius 决定 —— 它已烘进砖位，
    // `tileStartDist/tileEndDist` 就是它）。按活邻居重算半径会随 MoveTrack 位移伸缩，
    // 违反"只有 ScaleRadius 改半径"（用户 2026-10-30）。
    if (anim) livePivot(L, anim, tileIdx, tt, c.pivotX, c.pivotY);
    // 长按砖的滑动圆心**必须在这里按活枢轴重算**：缓存在 `loadTrailTileConsts` 里的那份
    // 是按"进砖那一刻"的枢轴算的，到达瞬间与活值错位 → 拖尾被拉成跨屏横带（踩过）。
    if (c.hasSlide) {
        const double exitDir = c.startAngle + c.totalAngle;
        const auto tail = L.tilePositionAt(tileIdx + 1);
        const double endCX = (double)tail[0] - std::cos(exitDir) * c.endDist;
        const double endCY = (double)tail[1] - std::sin(exitDir) * c.endDist;
        c.slideX = endCX - c.pivotX;
        c.slideY = endCY - c.pivotY;
    }
}

// `positionAtTile` 主分支的算术，逐字照抄（同操作、同顺序、同类型）。
// 唯一区别：`startTime/duration/startAngle/totalAngle/dist/pivot` 来自已提好的常量。
inline void trailPointAt(const TrailTileConsts& c, double tt, double& rx, double& ry,
                         double& bx, double& by) {
    double progress = (c.duration > 0.0) ? (tt - c.startTime) / c.duration : 1.0;
    if (progress < 0) progress = 0; if (progress > 1) progress = 1;
    const double angle = c.startAngle + c.totalAngle * progress;
    const double dist = c.startDist + (c.endDist - c.startDist) * progress;
    const double cx = c.pivotX + c.slideX * progress;
    const double cy = c.pivotY + c.slideY * progress;
    const double mvx = cx + std::cos(angle) * dist;
    const double mvy = cy + std::sin(angle) * dist;
    if (c.isRed) { rx = cx; ry = cy; bx = mvx; by = mvy; }
    else         { bx = cx; by = cy; rx = mvx; ry = mvy; }
}

}  // namespace

void PositionSolver::sampleTrailRange(const Timeline& timeline, double startTime, double t,
                                      float sampleRate, const glm::dvec2& redHead,
                                      const glm::dvec2& blueHead, std::vector<glm::dvec2>& redOut,
                                      std::vector<glm::dvec2>& blueOut, const TileAnimation* anim) {
    if (sampleRate <= 0.0f || t <= startTime) return;

    const int maxSamples = (int)std::ceil((t - startTime) * sampleRate) + 1;
    const double dt = 1.0 / sampleRate;

    int tileIdx = timeline.findTileIndex(startTime);
    if (tileIdx < 0) tileIdx = 0;
    const int tsz = (int)timeline.tileCount();
    const int64_t nFull = timeline.level()->tileCount();
    const int n = fitsInt32(nFull) ? (int)nFull : 0;

    // 直接写输出（原来是两条临时 double 数组 + 末尾一趟拷贝）。
    // 末尾的 head 也要一格，所以 +1。
    const size_t base = redOut.size();
    redOut.resize(base + (size_t)maxSamples + 1);
    blueOut.resize(base + (size_t)maxSamples + 1);
    size_t samples = 0;

    // `tileStartTimes()` 非空 = 没被窗口化（Timeline 丢掉那支数组后它是空的）——
    // 此时 `startTimeAt(i)` 就是读这条数组。极密谱（每采样点跨几十个砖）里推进循环是热点，
    // 直接扫数组比每次调 `startTimeAt()` 少一层分支/窗口查询；值完全一样（同一条数组）。
    const std::vector<double>& startArrVec = timeline.tileStartTimes();
    const bool denseStarts = startArrVec.size() == (size_t)tsz;
    const double* const startArr = denseStarts ? startArrVec.data() : nullptr;
    // 第 idx 块砖的起始时间（窗口化时走访问器；越界 = +inf，表示"没有下一砖"）
    auto startTimeOf = [&](int idx) -> double {
        if (idx < 0 || idx >= tsz) return std::numeric_limits<double>::infinity();
        return denseStarts ? startArr[idx] : timeline.startTimeAt(idx);
    };

    TrailTileConsts c;
    int cTile = -1;              // c 对应的砖号（-1 = 还没有；推进后置 -1 变懒加载）
    double tileStart = startTimeOf(tileIdx);
    double nextStart = startTimeOf(tileIdx + 1);

    for (int i = 0; i < maxSamples; i++) {
        double tt = startTime + dt * (double)i;
        if (tt > t) break;
        // 推进语义与原来逐字相同：`while (tileIdx + 1 < tsz && tt >= startTimeAt(tileIdx + 1))`。
        // 区别只是下一砖的起始时间被记下来了（它是纯函数，值不变），不必每采样点重取。
        while (tileIdx + 1 < tsz && tt >= nextStart) {
            tileIdx++;
            tileStart = nextStart;      // startTimeAt(旧 tileIdx + 1) == 新砖的 startTimeAt
            nextStart = startTimeOf(tileIdx + 1);
            cTile = -1;
        }
        if (cTile != tileIdx) {         // 每采样点最多一次
            loadTrailTileConsts(timeline, tileIdx, n, tileStart, c);
            cTile = tileIdx;
        }
        // 粘性轨道 + 轨道动画：**每个采样点**都要按 tt 重算枢轴/角度（拖尾必须解析式采样，
        // 不能拿"进砖那一刻"的常量）。没有位移的砖仍然走缓存常量 —— 与改造前逐位相同。
        loadTrailTileConstsLive(timeline, anim, tileIdx, tt, c);
        if (i < 3 && env::flag("ADOCAO_TRAIL_INPUTS")) {
            const LevelData& L = *timeline.level();
            LOG_I("trail-in[%d] idx=%d tt=%.17g st=%.17g pos=%.17g,%.17g sa=%.17g ta=%.17g dir=%.17g",
                  i, tileIdx, tt, c.startTime, c.pivotX, c.pivotY,
                  c.startAngle, c.totalAngle,
                  (double)L.tileDirectionAt((int64_t)tileIdx - 1));
        }
        double rx = 0.0, ry = 0.0, bx = 0.0, by = 0.0;
        // **拖尾 = 行星轨迹**，所以采样点必须由**算行星位置的同一个函数**给出。
        // 原来这里走 `trailPointAt`（一份"缓存副本"），为它已经踩过两次坑：
        //   ① 长按期间圆心在平移，副本不知道 → 拖尾是水平直线；
        //   ② 补了 slide 之后，到达瞬间缓存常量与活值错位 → 拖尾被拉成跨屏横带。
        // 现在一律调 `positionAtTile`（与行星逐位相同），慢一点换来"不可能分家"。
        if (c.tileMain && tt >= c.firstStart) {
            // 特例（首砖之前 / 末尾合成砖）：照原样调 positionAtTile，语义与逐位都不动。
            glm::dvec2 r(0), b(0);
            positionAtTile(timeline, tt, tileIdx, r, b, anim);
            rx = r.x; ry = r.y; bx = b.x; by = b.y;
        }
        redOut[base + samples]  = glm::dvec2(rx, ry);
        blueOut[base + samples] = glm::dvec2(bx, by);
        samples++;
    }
    // Bind trail head directly to planet's actual position.
    redOut[base + samples]  = glm::dvec2(redHead.x, redHead.y);
    blueOut[base + samples] = glm::dvec2(blueHead.x, blueHead.y);
    samples++;
    redOut.resize(base + samples);
    blueOut.resize(base + samples);
}

void PositionSolver::sampleTrail(const Timeline& timeline, double t, float trailDuration,
                                 float sampleRate, const glm::dvec2& redHead,
                                 const glm::dvec2& blueHead, std::vector<glm::dvec2>& redOut,
                                 std::vector<glm::dvec2>& blueOut, const TileAnimation* anim) {
    if (trailDuration <= 0.0f) return;
    sampleTrailRange(timeline, t - trailDuration, t, sampleRate, redHead, blueHead, redOut, blueOut,
                     anim);
}

}  // namespace adofai
