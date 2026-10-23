#pragma once

#include <glm/glm.hpp>

namespace adofai { class Camera; }   // 库侧类型：前置声明也要在 adofai 里

// Letterboxed viewport (in framebuffer pixels) for a target aspect ratio.
struct LetterboxedViewport {
    int x = 0, y = 0, w = 0, h = 0;
};

LetterboxedViewport computeLetterbox(int fbW, int fbH, float targetAspect);

// Camera navigation state machine (was GameWindow::Input / handleInput camera
// parts): drag-to-pan, wheel zoom, click detection and screen↔world math.
//
// The camera has a "rest" target (base) plus a live drag offset; while the
// user drags, update() converts the pixel delta to world units and re-targets
// the camera at base+offset. Playback drives the camera through snapTo()
// (playback follow), which cancels any drag offset first.
class CameraController {
public:
    CameraController() = default;

    void attach(adofai::Camera& camera) { m_camera = &camera; }

    // Move the camera to a world position: sets target, updates the pan base
    // and cancels any in-progress drag offset.
    void snapTo(double worldX, double worldY);

    // Mouse events (fed from GLFW callbacks; pressed = GLFW_PRESS).
    void onMouseButton(bool pressed);
    void onCursorPos(double x, double y);
    void onScroll(double yOffset);

    // True once after a press+release without drag movement (a "click").
    // Cleared by this call. Poll it only when playback is stopped.
    bool consumeClick();
    bool dragging() const { return m_dragging; }

    // Per-frame update. panEnabled=false while playing (playback owns the
    // camera); the drag state is kept so panning resumes seamlessly.
    // winW/winH = **窗口点**尺寸（光标坐标的坐标系）。传 -1 = 用上次 setWindowSizePts 的值。
    // 显式传入是主路径：GLFW 的 size 回调在创建早期不一定触发过，靠缓存值会拿到 0
    // （= 缩放锚点算到左上角，用户看到的就是"缩放中心在屏幕左上"）。
    void update(int fbW, int fbH, float targetAspect, bool panEnabled,
                int winW = -1, int winH = -1);

    // Convert a window pixel position to world coordinates (letterbox aware).
    glm::dvec2 screenToWorld(double x, double y, int fbW, int fbH, float targetAspect) const;

    // GLFW 给的是**窗口点**，而 `screenToWorld`/letterbox 用的是**帧缓冲像素**；
    // Retina（scale 2）和全屏下两者不相等 —— 不换算就会"缩放中心偏一半"。
    // 由宿主在初始化与 framebuffer-size 回调里喂进来。
    void setWindowSizePts(int w, int h) { m_winW = w; m_winH = h; }

    // 光标位置：窗口点（GLFW 回调原样）与帧缓冲像素（做几何换算用）。
    double cursorX() const { return m_cursorX; }
    double cursorY() const { return m_cursorY; }
    double cursorFbX(int fbW) const { return m_winW > 0 ? m_cursorX * (double)fbW / (double)m_winW : m_cursorX; }
    double cursorFbY(int fbH) const { return m_winH > 0 ? m_cursorY * (double)fbH / (double)m_winH : m_cursorY; }

private:
    adofai::Camera* m_camera = nullptr;

    double m_cursorX = 0, m_cursorY = 0;
    double m_dragStartX = 0, m_dragStartY = 0;
    double m_baseTargetX = 0, m_baseTargetY = 0;   // rest position (no drag)
    double m_offsetX = 0, m_offsetY = 0;           // live drag offset
    bool m_dragging = false;
    bool m_cursorSeen = false;
    int m_dragFrames = 0;
    double m_prevTargetX = 0.0, m_prevTargetY = 0.0;      // 光标进过窗口没有（没有就别做"以光标为中心"）
    double m_pendingScroll = 0.0;   // 累积到下一次 update()：滚轮要**以光标为中心**缩放
    int m_lastFbW = 0, m_lastFbH = 0;
    int m_winW = 0, m_winH = 0;   // 窗口点尺寸（换算光标坐标用）
    float m_lastAspect = 0.0f;
    bool m_clickPending = false;
};
