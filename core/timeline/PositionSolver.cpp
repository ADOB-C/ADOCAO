#include "core/timeline/PositionSolver.hpp"
#include <cstdlib>   // getenv（诊断）
#include "core/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>
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
        if (cfg.adaptive) rate = std::clamp(rate, (double)cfg.rateMin, (double)cfg.rateMax);
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

void PositionSolver::sampleTrailRange(const Timeline& timeline, double startTime, double t, float sampleRate, const glm::dvec2& redHead, const glm::dvec2& blueHead, std::vector<glm::dvec2>& redOut, std::vector<glm::dvec2>& blueOut) {
    if (sampleRate <= 0.0f || t <= startTime) return;

    const int maxSamples = (int)std::ceil((t - startTime) * sampleRate) + 1;
    const double dt = 1.0 / sampleRate;

    std::vector<double> redXY(maxSamples*2 + 2), blueXY(maxSamples*2 + 2);
    int samples = 0;

    int tileIdx = timeline.findTileIndex(startTime);
    if (tileIdx < 0) tileIdx = 0;
    const int tsz = (int)timeline.tileCount();


    for (int i = 0; i < maxSamples; i++) {
        double tt = startTime + dt * (double)i;
        if (tt > t) break;
        while (tileIdx + 1 < tsz && tt >= timeline.startTimeAt(tileIdx + 1))
            tileIdx++;
        if (i < 3 && env::flag("ADOCAO_TRAIL_INPUTS")) {
            const LevelData& L = *timeline.level();
            const auto p = L.tilePositionAt(tileIdx);
            LOG_I("trail-in[%d] idx=%d tt=%.17g st=%.17g pos=%.17g,%.17g sa=%.17g ta=%.17g dir=%.17g",
                  i, tileIdx, tt, timeline.startTimeAt(tileIdx), p[0], p[1],
                  (double)timeline.startAngleAt(tileIdx), (double)timeline.totalAngleAt(tileIdx),
                  (double)L.tileDirectionAt((int64_t)tileIdx - 1));
        }
        glm::dvec2 r(0), b(0);
        positionAtTile(timeline, tt, tileIdx, r, b);
        redXY[samples*2]=r.x; redXY[samples*2+1]=r.y;
        blueXY[samples*2]=b.x; blueXY[samples*2+1]=b.y;
        samples++;
    }
    {
        // Bind trail head directly to planet's actual position.
        redXY[samples*2]   = redHead.x;
        redXY[samples*2+1] = redHead.y;
        blueXY[samples*2]   = blueHead.x;
        blueXY[samples*2+1] = blueHead.y;
        samples++;
    }
    redOut.reserve(samples);
    blueOut.reserve(samples);
    for (int i = 0; i < samples; i++) {
        redOut.emplace_back(redXY[i*2], redXY[i*2+1]);
        blueOut.emplace_back(blueXY[i*2], blueXY[i*2+1]);
    }
}

void PositionSolver::sampleTrail(const Timeline& timeline, double t, float trailDuration, float sampleRate, const glm::dvec2& redHead, const glm::dvec2& blueHead, std::vector<glm::dvec2>& redOut, std::vector<glm::dvec2>& blueOut) {
    if (trailDuration <= 0.0f) return;
    sampleTrailRange(timeline, t - trailDuration, t, sampleRate, redHead, blueHead, redOut, blueOut);
}

}  // namespace adofai
