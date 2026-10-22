#include "core/timeline/PositionSolver.hpp"
#include <cstdlib>   // getenv（诊断）
#include "core/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <chrono>
#include "core/util/CountLimits.hpp"
#include "core/util/Logger.hpp"
#include "core/util/Env.hpp"


namespace adofai {

void PositionSolver::positionAt(const Timeline& timeline, double t, glm::dvec2& redOut, glm::dvec2& blueOut) {
    const LevelData& L = *timeline.level();   // ③ 切片 4：走访问器，不直接碰 tiles[]
    const int64_t nFull = L.tileCount();   // #0：int64 算，收窄前显式判
    if (!fitsInt32(nFull)) {
        LOG_E("PositionSolver 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32", (long long)nFull);
        return;
    }
    const int n = (int)nFull;
    if (n < 2) return;
    int tileIdx = timeline.findTileIndex(t);

    // Before first tile: orbit backward at constant BPM
    if (t < timeline.firstStartTime()) {
        const auto& p0 = L.tilePositionAt(0);
        double bpm = timeline.tileBPMs()[0];
        bool cw = timeline.tileIsCW()[0];
        double startAngle = (timeline.level()->settings.rotation + 180.0) * 3.14159265358979 / 180.0;
        double dt = timeline.firstStartTime() - t;
        double rps = (bpm / 60.0) * 3.14159265358979;
        double angle = cw ? (startAngle + dt * rps) : (startAngle - dt * rps);
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
        const auto& pivotPos = L.tilePositionAt(lastIdx);
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
    const auto& pivotPos = L.tilePositionAt(tileIdx);
    double startTime = timeline.startTimeAt(tileIdx);
    double duration = timeline.durationAt(tileIdx);
    double progress = (duration>0.0001)?(t-startTime)/duration:1.0;
    if (progress<0) progress=0; if (progress>1) progress=1;
    double angle = (double)timeline.startAngleAt(tileIdx)+(double)timeline.totalAngleAt(tileIdx)*progress;
    double dist = (double)timeline.tileStartDist(tileIdx)+((double)timeline.tileEndDist(tileIdx)-(double)timeline.tileStartDist(tileIdx))*progress;
    glm::dvec2 pv(pivotPos[0],pivotPos[1]);
    glm::dvec2 mv(pivotPos[0]+std::cos(angle)*dist, pivotPos[1]+std::sin(angle)*dist);
    if (isRed) { redOut=pv; blueOut=mv; }
    else       { blueOut=pv; redOut=mv; }
}

void PositionSolver::positionAtTile(const Timeline& timeline, double t, int tileIdx, glm::dvec2& redOut, glm::dvec2& blueOut) {
    const LevelData& L = *timeline.level();   // ③ 切片 4：走访问器，不直接碰 tiles[]
    const int64_t nFull = L.tileCount();   // #0：int64 算，收窄前显式判
    if (!fitsInt32(nFull)) {
        LOG_E("PositionSolver 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32", (long long)nFull);
        return;
    }
    const int n = (int)nFull;
    if (n < 2) return;
    if (t < timeline.firstStartTime()) {
        const auto& p0 = L.tilePositionAt(0);
        double bpm = timeline.tileBPMs()[0]; bool cw = timeline.tileIsCW()[0];
        double startAngle = (timeline.level()->settings.rotation + 180.0) * 3.14159265358979 / 180.0;
        double dts = timeline.firstStartTime() - t;
        double rps = (bpm / 60.0) * 3.14159265358979;
        double angle = cw ? (startAngle + dts * rps) : (startAngle - dts * rps);
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
        const auto& pivotPos = L.tilePositionAt(lastIdx);
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
    const auto& pivotPos = L.tilePositionAt(tileIdx);
    double startTime = timeline.startTimeAt(tileIdx);
    double duration = timeline.durationAt(tileIdx);
    double progress = (duration>0.0001)?(t-startTime)/duration:1.0;
    if (progress<0) progress=0; if (progress>1) progress=1;
    double angle = (double)timeline.startAngleAt(tileIdx)+(double)timeline.totalAngleAt(tileIdx)*progress;
    double dist = (double)timeline.tileStartDist(tileIdx)+((double)timeline.tileEndDist(tileIdx)-(double)timeline.tileStartDist(tileIdx))*progress;
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
    const int idx = timeline.findTileIndex(t);
    return std::max((double)timeline.tileStartDist(idx), (double)timeline.tileEndDist(idx));
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
        if (cfg.adaptive || cfg.sagittaPx > 0.0f)
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
            LOG_W("trail: rate × span 超过 maxPoints=%.0f（span=%.3fs）→ 实际 %.1f/s（告警去重 2s）",
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
    c.startDist = (double)timeline.tileStartDist(tileIdx);
    c.endDist = (double)timeline.tileEndDist(tileIdx);
    c.isRed = (tileIdx % 2 == 0);
    c.firstStart = timeline.firstStartTime();
    // 与 `positionAtTile` 的分支判据一致：n < 2 或首砖之前走特例；末尾合成砖（>= n-1）也走特例。
    c.tileMain = (n >= 2) && (tileIdx < n - 1);
}

// `positionAtTile` 主分支的算术，逐字照抄（同操作、同顺序、同类型）。
// 唯一区别：`startTime/duration/startAngle/totalAngle/dist/pivot` 来自已提好的常量。
inline void trailPointAt(const TrailTileConsts& c, double tt, double& rx, double& ry,
                         double& bx, double& by) {
    double progress = (c.duration > 0.0001) ? (tt - c.startTime) / c.duration : 1.0;
    if (progress < 0) progress = 0; if (progress > 1) progress = 1;
    const double angle = c.startAngle + c.totalAngle * progress;
    const double dist = c.startDist + (c.endDist - c.startDist) * progress;
    const double mvx = c.pivotX + std::cos(angle) * dist;
    const double mvy = c.pivotY + std::sin(angle) * dist;
    if (c.isRed) { rx = c.pivotX; ry = c.pivotY; bx = mvx; by = mvy; }
    else         { bx = c.pivotX; by = c.pivotY; rx = mvx; ry = mvy; }
}

}  // namespace

void PositionSolver::sampleTrailRange(const Timeline& timeline, double startTime, double t, float sampleRate, const glm::dvec2& redHead, const glm::dvec2& blueHead, std::vector<glm::dvec2>& redOut, std::vector<glm::dvec2>& blueOut) {
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
        if (i < 3 && env::flag("ADOCAO_TRAIL_INPUTS")) {
            const LevelData& L = *timeline.level();
            LOG_I("trail-in[%d] idx=%d tt=%.17g st=%.17g pos=%.17g,%.17g sa=%.17g ta=%.17g dir=%.17g",
                  i, tileIdx, tt, c.startTime, c.pivotX, c.pivotY,
                  c.startAngle, c.totalAngle,
                  (double)L.tileDirectionAt((int64_t)tileIdx - 1));
        }
        double rx = 0.0, ry = 0.0, bx = 0.0, by = 0.0;
        if (c.tileMain && tt >= c.firstStart) {
            trailPointAt(c, tt, rx, ry, bx, by);
        } else {
            // 特例（首砖之前 / 末尾合成砖）：照原样调 positionAtTile，语义与逐位都不动。
            glm::dvec2 r(0), b(0);
            positionAtTile(timeline, tt, tileIdx, r, b);
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

void PositionSolver::sampleTrail(const Timeline& timeline, double t, float trailDuration, float sampleRate, const glm::dvec2& redHead, const glm::dvec2& blueHead, std::vector<glm::dvec2>& redOut, std::vector<glm::dvec2>& blueOut) {
    if (trailDuration <= 0.0f) return;
    sampleTrailRange(timeline, t - trailDuration, t, sampleRate, redHead, blueHead, redOut, blueOut);
}

}  // namespace adofai
