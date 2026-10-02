#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

void PositionSolver::positionAt(const Timeline& timeline, double t, glm::dvec2& redOut, glm::dvec2& blueOut) {
    const auto& tiles = timeline.level()->tiles;
    int n = (int)tiles.size();
    if (n < 2) return;
    int tileIdx = timeline.findTileIndex(t);

    // Before first tile: orbit backward at constant BPM
    if (t < timeline.tileStartTimes()[0]) {
        const auto& p0 = tiles[0].position;
        double bpm = timeline.tileBPMs()[0];
        bool cw = timeline.tileIsCW()[0];
        double startAngle = (timeline.level()->settings.rotation + 180.0) * 3.14159265358979 / 180.0;
        double dt = timeline.tileStartTimes()[0] - t;
        double rps = (bpm / 60.0) * 3.14159265358979;
        double angle = cw ? (startAngle + dt * rps) : (startAngle - dt * rps);
        double dist = 1.0;
        glm::dvec2 mv(p0[0] + std::cos(angle) * dist, p0[1] + std::sin(angle) * dist);
        glm::dvec2 pv(p0[0], p0[1]);
        if (0 % 2 == 0) { redOut = pv; blueOut = mv; }
        else            { blueOut = pv; redOut = mv; }
        return;
    }

    if (tileIdx >= n - 1) {
        int lastIdx = n - 1;
        const auto& pivotPos = tiles[lastIdx].position;
        double startAngle = 0.0;
        if (lastIdx > 0) {
            const auto& prevPos = tiles[lastIdx - 1].position;
            startAngle = std::atan2(prevPos[1]-pivotPos[1], prevPos[0]-pivotPos[0]);
        }
        double extraTime = t - timeline.tileStartTimes()[lastIdx];
        double rps = (double)(timeline.tileBPMs()[lastIdx]/60.0) * 3.14159265358979;
        double currentAngle = timeline.tileIsCW()[lastIdx] ? (startAngle-extraTime*rps) : (startAngle+extraTime*rps);
        glm::dvec2 mv(pivotPos[0]+std::cos(currentAngle), pivotPos[1]+std::sin(currentAngle));
        if (lastIdx%2==0) { redOut=glm::dvec2(pivotPos[0],pivotPos[1]); blueOut=mv; }
        else              { blueOut=glm::dvec2(pivotPos[0],pivotPos[1]); redOut=mv; }
        return;
    }

    bool isRed = (tileIdx%2==0);
    const auto& pivotPos = tiles[tileIdx].position;
    double startTime = timeline.tileStartTimes()[tileIdx];
    double duration = timeline.tileDurations()[tileIdx];
    double progress = (duration>0.0001)?(t-startTime)/duration:1.0;
    if (progress<0) progress=0; if (progress>1) progress=1;
    double angle = (double)timeline.tileStartAngles()[tileIdx]+(double)timeline.tileTotalAngles()[tileIdx]*progress;
    double dist = (double)timeline.tileStartDist()[tileIdx]+((double)timeline.tileEndDist()[tileIdx]-(double)timeline.tileStartDist()[tileIdx])*progress;
    glm::dvec2 pv(pivotPos[0],pivotPos[1]);
    glm::dvec2 mv(pivotPos[0]+std::cos(angle)*dist, pivotPos[1]+std::sin(angle)*dist);
    if (isRed) { redOut=pv; blueOut=mv; }
    else       { blueOut=pv; redOut=mv; }
}

void PositionSolver::positionAtTile(const Timeline& timeline, double t, int tileIdx, glm::dvec2& redOut, glm::dvec2& blueOut) {
    const auto& tiles = timeline.level()->tiles;
    int n = (int)tiles.size();
    if (n < 2) return;
    if (t < timeline.tileStartTimes()[0]) {
        const auto& p0 = tiles[0].position;
        double bpm = timeline.tileBPMs()[0]; bool cw = timeline.tileIsCW()[0];
        double startAngle = (timeline.level()->settings.rotation + 180.0) * 3.14159265358979 / 180.0;
        double dts = timeline.tileStartTimes()[0] - t;
        double rps = (bpm / 60.0) * 3.14159265358979;
        double angle = cw ? (startAngle + dts * rps) : (startAngle - dts * rps);
        glm::dvec2 mv(p0[0] + std::cos(angle), p0[1] + std::sin(angle));
        glm::dvec2 pv(p0[0], p0[1]);
        if (0 % 2 == 0) { redOut = pv; blueOut = mv; }
        else            { blueOut = pv; redOut = mv; }
        return;
    }
    if (tileIdx >= n - 1) {
        int lastIdx = n - 1;
        const auto& pivotPos = tiles[lastIdx].position;
        double startAngle = 0.0;
        if (lastIdx > 0) {
            const auto& prevPos = tiles[lastIdx - 1].position;
            startAngle = std::atan2(prevPos[1]-pivotPos[1], prevPos[0]-pivotPos[0]);
        }
        double extraTime = t - timeline.tileStartTimes()[lastIdx];
        double rps = (double)(timeline.tileBPMs()[lastIdx]/60.0) * 3.14159265358979;
        double currentAngle = timeline.tileIsCW()[lastIdx] ? (startAngle-extraTime*rps) : (startAngle+extraTime*rps);
        glm::dvec2 mv(pivotPos[0]+std::cos(currentAngle), pivotPos[1]+std::sin(currentAngle));
        if (lastIdx%2==0) { redOut=glm::dvec2(pivotPos[0],pivotPos[1]); blueOut=mv; }
        else              { blueOut=glm::dvec2(pivotPos[0],pivotPos[1]); redOut=mv; }
        return;
    }
    bool isRed = (tileIdx%2==0);
    const auto& pivotPos = tiles[tileIdx].position;
    double startTime = timeline.tileStartTimes()[tileIdx];
    double duration = timeline.tileDurations()[tileIdx];
    double progress = (duration>0.0001)?(t-startTime)/duration:1.0;
    if (progress<0) progress=0; if (progress>1) progress=1;
    double angle = (double)timeline.tileStartAngles()[tileIdx]+(double)timeline.tileTotalAngles()[tileIdx]*progress;
    double dist = (double)timeline.tileStartDist()[tileIdx]+((double)timeline.tileEndDist()[tileIdx]-(double)timeline.tileStartDist()[tileIdx])*progress;
    glm::dvec2 pv(pivotPos[0],pivotPos[1]);
    glm::dvec2 mv(pivotPos[0]+std::cos(angle)*dist, pivotPos[1]+std::sin(angle)*dist);
    if (isRed) { redOut=pv; blueOut=mv; }
    else       { blueOut=pv; redOut=mv; }
}

double PositionSolver::tilePathSpeed(const Timeline& timeline, double t) {
    const auto& durations = timeline.tileDurations();
    if (durations.empty()) return 0.0;
    int idx = timeline.findTileIndex(t);
    if (idx < 0) idx = 0;
    if (idx >= (int)durations.size()) idx = (int)durations.size() - 1;
    const double d = durations[idx];
    if (d <= 1e-9) return 0.0;

    // Track covered by one tile: at least the step to the next tile centre, plus
    // the arc swept by the tile's relative angle (a 180 deg / midspin tile makes
    // the planet travel a long arc while barely advancing). Sampling has to
    // follow that arc, otherwise slow sharp turns come out as chunky polylines.
    const double step = 1.0;
    const double rot  = std::abs((double)timeline.tileTotalAngles()[idx]);
    const double radius = std::max((double)timeline.tileStartDist()[idx],
                                   (double)timeline.tileEndDist()[idx]);
    const double arc = rot * std::max(0.5, radius);
    return std::max(step, arc) / d;
}

void PositionSolver::sampleTrail(const Timeline& timeline, double t, float trailDuration, float sampleRate, const glm::dvec2& redHead, const glm::dvec2& blueHead, std::vector<glm::dvec2>& redOut, std::vector<glm::dvec2>& blueOut) {
    if (sampleRate <= 0.0f || trailDuration <= 0.0f) return;

    const int maxSamples = (int)std::ceil(trailDuration * sampleRate) + 1;
    const double dt = 1.0 / sampleRate;

    std::vector<double> redXY(maxSamples*2 + 2), blueXY(maxSamples*2 + 2);
    int samples = 0;

    double startTime = t - trailDuration;
    int tileIdx = timeline.findTileIndex(startTime);
    if (tileIdx < 0) tileIdx = 0;
    int tsz = (int)timeline.tileStartTimes().size();

    for (int i = 0; i < maxSamples; i++) {
        double tt = startTime + dt * (double)i;
        if (tt > t) break;
        while (tileIdx + 1 < tsz && tt >= timeline.tileStartTimes()[tileIdx + 1])
            tileIdx++;
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
