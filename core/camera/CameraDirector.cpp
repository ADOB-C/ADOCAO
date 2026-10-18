#include "core/camera/CameraDirector.hpp"
#include "core/util/Logger.hpp"
#include "core/util/Env.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adofai {

namespace {

constexpr double kTileSize = 1.0;   // Re_ADOJAS 的 TILE_SIZE（位置全部以"砖"为单位）

inline double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }

// `settings.relativeTo` 的解析：Re_ADOJAS 是 `parseMovementType(s.relativeTo) ?? 'Player'`，
// 也就是**认不得就按 Player**（与事件里的未知字符串不同 —— 那里是"沿用上一次模式"）。
uint8_t settingsRelativeTo(const std::string& s) {
    if (s == "Tile") return LevelData::CamTile;
    if (s == "Global") return LevelData::CamGlobal;
    if (s == "LastPosition") return LevelData::CamLastPosition;
    if (s == "LastPositionNoRotation") return LevelData::CamLastPositionNoRotation;
    return LevelData::CamPlayer;
}

}  // namespace

void CameraDirector::reset(const LevelData::Settings& settings,
                           const std::vector<CameraKey>& keys,
                           double floor0X, double floor0Y, float bpm0) {
    m_settings = settings;
    m_keys = &keys;
    m_floor0X = floor0X;
    m_floor0Y = floor0Y;
    m_bpm0 = bpm0;
    resetState();
}

void CameraDirector::resetState() {
    m_legacyRelativeTo = m_settings.legacyCamRelativeTo;

    m_moveX = Tween{};
    m_moveY = Tween{};
    m_rotationTween = Tween{};
    m_zoomTween = Tween{};

    m_camParent = Vec{};
    m_camAngle = 0.0;
    m_zoomSize = 1.0;

    m_frompos = Vec{};
    m_topos = Vec{};
    m_pos = Vec{};
    m_timer = 0.0;
    m_followMode = true;

    m_lastEventRelativePosition = Vec{};
    m_lastUsedMovementType = LevelData::CamPlayer;
    m_lastTileCamFloor = -1;
    m_lastFollowTile = -1;

    m_cursor = -1;

    // 合成事件：**settings 才是相机的初始值**，MoveCamera 事件只是后续覆盖。
    CameraKey syn;
    syn.time = 0.0;
    syn.anchorX = m_floor0X;
    syn.anchorY = m_floor0Y;
    syn.bpm = m_bpm0;
    LevelData::CameraEvent& ev = syn.ev;
    ev.floor = 0;
    ev.duration = 0.0f;
    ev.x = m_settings.position[0];
    ev.y = m_settings.position[1];
    ev.rotation = m_settings.rotation;
    ev.zoom = m_settings.zoom;
    ev.relativeTo = settingsRelativeTo(m_settings.relativeTo);
    ev.ease = (uint8_t)Ease::Linear;
    ev.flags = LevelData::kCamHasPosition | LevelData::kCamHasX | LevelData::kCamHasY |
               LevelData::kCamHasRotation | LevelData::kCamHasZoom;
    startEffect(syn, 0.0, 0.0, 0.0, 1.0);
}

void CameraDirector::seek(double t, double planetX, double planetY, int64_t tileIndex, double pitch) {
    if (env::flag("ADOCAO_CAM_DEBUG")) {
        LOG_I("cam-seek: t=%.3f in=(%.2f,%.2f) tile=%lld keys=%zu", t, planetX, planetY,
              (long long)tileIndex, m_keys ? m_keys->size() : 0);
    }
    resetState();
    int64_t idx = -1;
    if (m_keys) {
        for (size_t i = 0; i < m_keys->size(); i++) {
            const CameraKey& k = (*m_keys)[i];
            if (k.time > t) break;
            idx = (int64_t)i;
            applyTweens(k.time);
            // 循环里把跟随层钉在行星上：让 free→follow 的换算看到确定的世界坐标
            if (m_followMode) { m_frompos = m_topos = m_pos = Vec{planetX, planetY}; }
            startEffect(k, planetX, planetY, k.time, pitch);
        }
    }
    m_cursor = idx;

    applyTweens(t);
    if (m_followMode) {
        m_frompos = m_topos = m_pos = Vec{planetX, planetY};
        m_timer = 1e9;                 // 稳态：续播的第一帧不会再追一遍
    } else {
        m_frompos = m_topos = m_pos = Vec{};
        m_timer = 0.0;
    }
    m_lastFollowTile = tileIndex;
}

void CameraDirector::update(double nowSeconds, double deltaSeconds, float bpm, double pitch,
                            double planetX, double planetY, int64_t tileIndex) {
    // 回退守卫：时间倒退到"已处理事件之前"就整体重置（游标 -1 → 本帧从第一个事件重放）
    if (m_keys && m_cursor >= 0 && (size_t)m_cursor < m_keys->size() &&
        nowSeconds < (*m_keys)[(size_t)m_cursor].time) {
        resetState();
    }

    if (m_keys) {
        int64_t idx = m_cursor;
        while (idx + 1 < (int64_t)m_keys->size() &&
               (*m_keys)[(size_t)(idx + 1)].time <= nowSeconds) {
            idx++;
            const CameraKey& k = (*m_keys)[(size_t)idx];
            applyTweens(k.time);       // 旧 tween 先求值到"这个事件的时刻"
            startEffect(k, planetX, planetY, k.time, pitch);
        }
        m_cursor = idx;
    }

    // UpdateFollowCam：只在切砖时重设目标（不是每帧）
    if (tileIndex != m_lastFollowTile) {
        m_lastFollowTile = tileIndex;
        updateFollowCam(planetX, planetY);
    }

    // camspeed = 60 / (bpm * pitch) * 2（**当前砖**的 bpm；tileBPM 已折进 SetSpeed）
    const double effBpm = std::max((double)bpm, 1e-6);
    const double effPitch = std::max(pitch, 1e-6);
    const double camspeed = (60.0 / (effBpm * effPitch)) * 2.0;

    m_timer += deltaSeconds;           // 先加：切砖那一帧就吃掉一个 dt（实测行为）

    applyTweens(nowSeconds);

    if (m_followMode) {
        const double t = clamp01(m_timer / camspeed);
        const double tx = m_topos.x;
        const double ty = m_topos.y;
        m_pos.x = m_frompos.x + (tx - m_frompos.x) * t;
        m_pos.y = m_frompos.y + (ty - m_frompos.y) * t;
    } else {
        m_pos = Vec{};
    }
}

// ── 事件 → 4 条 tween（startEffect，逐行对应 Re_ADOJAS） ───────────────────

void CameraDirector::startEffect(const CameraKey& k, double planetX, double planetY,
                                 double now, double pitch) {
    using C = LevelData;
    const C::CameraEvent& ev = k.ev;
    if (ev.flags & C::kCamInactive) return;

    const double effBpm = std::max((double)k.bpm, 1e-6);
    const double effPitch = std::max(pitch, 1e-6);
    const double duration = (double)ev.duration * (60.0 / (effBpm * effPitch));
    const Ease ease = (Ease)ev.ease;

    const bool posEnabled = (ev.disabled & C::kCamDisPosition) == 0;
    const bool positionUsed = (ev.flags & C::kCamHasPosition) && posEnabled;
    const double tx = (posEnabled && (ev.flags & C::kCamHasX)) ? (double)ev.x * kTileSize
                                                              : std::numeric_limits<double>::quiet_NaN();
    const double ty = (posEnabled && (ev.flags & C::kCamHasY)) ? (double)ev.y * kTileSize
                                                              : std::numeric_limits<double>::quiet_NaN();

    const bool rotationUsed = (ev.flags & C::kCamHasRotation) && (ev.disabled & C::kCamDisRotation) == 0;
    const bool zoomUsed = (ev.flags & C::kCamHasZoom) && (ev.disabled & C::kCamDisZoom) == 0;

    bool relativeToUsed = ev.relativeTo != C::CamAbsent && (ev.disabled & C::kCamDisRel) == 0;
    const uint8_t movementType = ev.relativeTo;   // 可能是 CamUnknown(0xFE)

    // ── 去重：同模式 + 同砖(Tile) 且 position 是 [null,null] → 这次 relativeTo 不生效
    if (relativeToUsed &&
        movementType != C::CamGlobal && movementType != C::CamLastPosition &&
        movementType != C::CamLastPositionNoRotation &&
        positionUsed && (std::isnan(tx) || std::isnan(ty)) &&
        movementType == m_lastUsedMovementType &&
        (movementType != C::CamTile || ev.floor == m_lastTileCamFloor)) {
        relativeToUsed = false;
    }

    const bool isLastPosition = (movementType == C::CamLastPosition ||
                                 movementType == C::CamLastPositionNoRotation);

    // ── Kill(complete: true)：旧 tween 直接跳到它的终点
    if (positionUsed || relativeToUsed) {
        if (!std::isnan(tx) || relativeToUsed) killTween(m_moveX, m_camParent.x);
        if (!std::isnan(ty) || relativeToUsed) killTween(m_moveY, m_camParent.y);
    }
    if (rotationUsed || (relativeToUsed && isLastPosition)) killTween(m_rotationTween, m_camAngle);
    if (zoomUsed) killTween(m_zoomTween, m_zoomSize);

    // ── NaN 归零：只要 relativeTo 生效，未写的分量归零（不带 relativeTo 时该轴保持不动）
    Vec vector{tx, ty};
    if (relativeToUsed) {
        if (std::isnan(vector.x)) vector.x = 0.0;
        if (std::isnan(vector.y)) vector.y = 0.0;
    }

    const uint8_t camMovementType =
        (relativeToUsed && movementType <= C::CamLastPositionNoRotation) ? movementType
                                                                        : m_lastUsedMovementType;

    const Vec camParentBefore = m_camParent;
    const Vec vector2 = positionUsed
        ? vector
        : Vec{m_lastEventRelativePosition.x - camParentBefore.x,
              m_lastEventRelativePosition.y - camParentBefore.y};

    Vec finalPos{};
    double rotationOffset = 0.0;

    if (m_legacyRelativeTo && !relativeToUsed) {
        finalPos = positionUsed ? Vec{vector.x + camParentBefore.x, vector.y + camParentBefore.y}
                                : camParentBefore;
    } else {
        switch (camMovementType) {
        case C::CamPlayer: {
            if (!m_followMode) {
                const Vec position = m_camParent;
                m_camParent.x = position.x - planetX;
                m_camParent.y = position.y - planetY;
                m_pos = Vec{planetX, planetY};
                m_followMode = true;
                m_frompos = m_pos;
                m_topos = Vec{planetX, planetY};
                m_timer = 0.0;
            }
            finalPos = vector2;
            break;
        }
        case C::CamTile: {
            if (m_followMode) {
                const Vec camWorld{cameraWorldX(), m_camParent.y + m_pos.y};
                m_camParent = camWorld;
                setToFreeMode();
            }
            m_lastEventRelativePosition = Vec{k.anchorX, k.anchorY};
            m_lastTileCamFloor = ev.floor;
            finalPos = Vec{vector2.x + k.anchorX, vector2.y + k.anchorY};
            break;
        }
        case C::CamGlobal: {
            if (m_followMode) {
                const Vec camWorld{cameraWorldX(), m_camParent.y + m_pos.y};
                m_camParent = camWorld;
                setToFreeMode();
            }
            m_lastEventRelativePosition = Vec{};
            finalPos = vector2;
            break;
        }
        case C::CamLastPosition:
        case C::CamLastPositionNoRotation: {
            const Vec vector4 = m_camParent;
            if (camMovementType == C::CamLastPosition) rotationOffset = m_camAngle;
            finalPos = positionUsed ? Vec{vector.x + vector4.x, vector.y + vector4.y} : vector4;
            break;
        }
        default:
            finalPos = vector2;
            break;
        }
    }

    if (relativeToUsed && movementType <= C::CamLastPositionNoRotation) {
        m_lastUsedMovementType = movementType;
    }

    // ── 建 tween（4 条独立；未写的轴不建）
    if (positionUsed || relativeToUsed) {
        if (!std::isnan(finalPos.x)) startTween(m_moveX, m_camParent.x, m_camParent.x, finalPos.x, duration, ease, now);
        if (!std::isnan(finalPos.y)) startTween(m_moveY, m_camParent.y, m_camParent.y, finalPos.y, duration, ease, now);
    }
    if (rotationUsed || (relativeToUsed && isLastPosition)) {
        const double targetRot = rotationUsed ? (double)ev.rotation : 0.0;
        startTween(m_rotationTween, m_camAngle, m_camAngle, targetRot + rotationOffset, duration, ease, now);
    }
    if (rotationUsed && env::flag("ADOCAO_CAM_DEBUG"))
        LOG_I("cam-rot: t=%.4f floor=%d target=%.2f from=%.2f dur=%.4f relOff=%.2f",
              now, (int)ev.floor, (double)ev.rotation, m_camAngle, duration, rotationOffset);
    if (zoomUsed) {
        startTween(m_zoomTween, m_zoomSize, m_zoomSize, (double)ev.zoom / 100.0, duration, ease, now);
    }
}

void CameraDirector::killTween(Tween& tw, double& dst) {
    if (!tw.active) return;
    dst = tw.to;
    tw.active = false;
}

void CameraDirector::startTween(Tween& tw, double& dst, double from, double to,
                                double duration, Ease ease, double now) {
    tw.from = from;
    tw.to = to;
    tw.ease = ease;
    if (duration > 0.0) {
        tw.active = true;
        tw.startTime = now;
        tw.duration = duration;
    } else {
        tw.active = false;
        tw.startTime = now;
        tw.duration = 0.0;
        dst = to;
    }
}

double CameraDirector::evalTween(const Tween& tw, double now) const {
    if (!tw.active) return tw.to;
    if (tw.duration <= 0.0) return tw.to;
    const double p = clamp01((now - tw.startTime) / tw.duration);
    return tw.from + (tw.to - tw.from) * easeApply(tw.ease, p);
}

void CameraDirector::applyTweens(double now) {
    const auto step = [&](Tween& tw, double& dst) {
        if (!tw.active) return;
        dst = evalTween(tw, now);
        if (now >= tw.startTime + tw.duration) tw.active = false;
    };
    step(m_moveX, m_camParent.x);
    step(m_moveY, m_camParent.y);
    step(m_rotationTween, m_camAngle);
    step(m_zoomTween, m_zoomSize);
}

void CameraDirector::setToFreeMode() {
    m_frompos = Vec{};
    m_topos = Vec{};
    m_pos = Vec{};
    m_followMode = false;
}

void CameraDirector::updateFollowCam(double planetX, double planetY) {
    if (!m_followMode) return;
    m_frompos = m_pos;
    m_topos = Vec{planetX, planetY};
    m_timer = 0.0;
}

}  // namespace adofai
