#pragma once

#include <cstdint>

#include "core/camera/CameraTimeline.hpp"
#include "core/level/LevelData.hpp"
#include "core/util/Easing.hpp"

namespace adofai {

// ADOFAI 的两层摄像机（Re_ADOJAS `CameraController` 的逐行移植，权威语义见 docs/camera.md）。
//
//   camParent（rig，绝对世界坐标 / Player 模式下是行星相对偏移）  ← MoveCamera 的 4 条 tween
//   pos（跟随层）                                                ← 每块砖一次 2 拍线性追赶
//   相机世界坐标 = camParent + pos
//
// 旋转（度）与缩放（百分比）不参与上面那个和。所有数值 double（浮点精度规则）。
// 这个类**不碰 GL**：app 每帧把结果写进 render/Camera，测试直接对拍数值。
class CameraDirector {
public:
    // 把时间线与关卡起点信息挂上（时间线必须是已展开、已排序的 CameraTimeline::keys()）。
    // 换关卡/重开时必须重新调用（会重置所有状态）。
    void reset(const LevelData::Settings& settings,
               const std::vector<CameraKey>& keys,
               double floor0X, double floor0Y, float bpm0);

    // 回到"文件 settings 给的初始状态"（合成一个 duration=0 的 MoveCamera）。
    void resetState();

    // 跳到时间 t（暂停/从中间开始播）：重放全部事件，跟随层强制稳态。
    //
    // `planetAt(t, x, y)`（可选）：**重放历史事件时**行星在该事件时刻的位置。
    // 为什么必须有它：`relativeTo: Player` 的事件把"行星位置"当基准换算世界坐标，
    // 若整个重放都用**当前**行星位置，历史事件就会把 rig 算到错误的地方 ——
    // 表现是"跳转/抓帧后相机整体偏移、旋转中心也不对"（2026-10-24 MM2 真踩过）。
    using PlanetAtFn = std::function<void(double t, double& x, double& y)>;
    void seek(double t, double planetX, double planetY, int64_t tileIndex, double pitch,
              const PlanetAtFn& planetAt = {});

    // 每帧驱动。bpm = **当前砖**的 BPM（跟随速度用）；pitch 已归一成倍率（settings.pitch/100）。
    void update(double nowSeconds, double deltaSeconds, float bpm, double pitch,
                double planetX, double planetY, int64_t tileIndex);

    // **宿主只用这一个**：内部决定"seek 还是 update"。判据原本在 ADOCAO 的 GameWindow 里
    // （首帧 / 跳转 / 回退 / 帧间隔 > 0.25 s → seek），下游（ADOFAO-E）复制过一份 → 搬进来。
    // 顺序敏感：seek 会重放全部事件并强制跟随层稳态，update 只推进 tween。
    void drive(double t, double deltaSeconds, float bpm, double pitch,
               double planetX, double planetY, int64_t tileIndex) {
        constexpr double kSeekGap = 0.25;   // 正常的帧间隔远小于它
        if (!m_driveActive || t < m_driveLastTime - 1e-6 || t - m_driveLastTime > kSeekGap) {
            seek(t, planetX, planetY, tileIndex, pitch);
            m_driveActive = true;
        } else {
            update(t, deltaSeconds, bpm, pitch, planetX, planetY, tileIndex);
        }
        m_driveLastTime = t;
    }
    // 换关卡/重开时调用（下一次 drive 会 seek）。
    void resetDrive() { m_driveActive = false; m_driveLastTime = 0.0; }
    bool driveActive() const { return m_driveActive; }

    // 长按带：跟随目标叠加的圆心滑动量（每帧由 app 写；非长按砖写 0）。

    // ── 结果 ───────────────────────────────────────────────────────────────
    double x() const { return m_camParent.x + m_pos.x; }
    double y() const { return m_camParent.y + m_pos.y; }
    double rotation() const { return m_camAngle; }          // 度
    double zoom() const { return m_zoomSize * 100.0; }      // ADOFAI 口径（100 = 默认）
    bool followMode() const { return m_followMode; }

private:
    struct Vec { double x = 0.0, y = 0.0; };
    struct Tween {
        bool   active = false;
        double startTime = 0.0;
        double duration = 0.0;
        double from = 0.0;
        double to = 0.0;
        Ease   ease = Ease::Linear;
    };

    void startEffect(const CameraKey& k, double planetX, double planetY, double now, double pitch);
    void killTween(Tween& tw, double& dst);
    void startTween(Tween& tw, double& dst, double from, double to,
                    double duration, Ease ease, double now);
    void applyTweens(double now);
    double evalTween(const Tween& tw, double now) const;
    void setToFreeMode();
    void updateFollowCam(double planetX, double planetY);
    double cameraWorldX() const { return m_camParent.x + m_pos.x; }

    bool   m_driveActive = false;   // drive() 的状态：false = 下一次必须 seek
    double m_driveLastTime = 0.0;

    // 关卡起点（reset 时缓存；resetState/回退守卫要用）
    LevelData::Settings m_settings{};
    const std::vector<CameraKey>* m_keys = nullptr;
    double m_floor0X = 0.0, m_floor0Y = 0.0;
    float  m_bpm0 = 100.0f;

    // rig
    Vec    m_camParent;
    double m_camAngle = 0.0;
    double m_zoomSize = 1.0;

    // 跟随层
    Vec    m_frompos, m_topos, m_pos;
    double m_timer = 0.0;
    bool   m_followMode = true;

    // 记账
    Vec     m_lastEventRelativePosition;
    uint8_t m_lastUsedMovementType = LevelData::CamPlayer;
    int32_t m_lastTileCamFloor = -1;
    int64_t m_lastFollowTile = -1;
    bool    m_legacyRelativeTo = false;

    // 4 条独立 tween
    Tween m_moveX, m_moveY, m_rotationTween, m_zoomTween;

    int64_t m_cursor = -1;   // 已处理到的下标（-1 = 一条都没处理过，与 Re_ADOJAS 同）
};

}  // namespace adofai
