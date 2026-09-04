#pragma once

#include <glm/glm.hpp>

class Camera;

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

    void attach(Camera& camera) { m_camera = &camera; }
    Camera& camera() const { return *m_camera; }

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

    // Per-frame update. panEnabled=false while playing (playback owns the
    // camera); the drag state is kept so panning resumes seamlessly.
    void update(int fbW, int fbH, float targetAspect, bool panEnabled);

    // Convert a window pixel position to world coordinates (letterbox aware).
    glm::dvec2 screenToWorld(double x, double y, int fbW, int fbH, float targetAspect) const;

    // Current cursor position in window pixels.
    double cursorX() const { return m_cursorX; }
    double cursorY() const { return m_cursorY; }
    bool dragging() const { return m_dragging; }

private:
    Camera* m_camera = nullptr;

    double m_cursorX = 0, m_cursorY = 0;
    double m_dragStartX = 0, m_dragStartY = 0;
    double m_baseTargetX = 0, m_baseTargetY = 0;   // rest position (no drag)
    double m_offsetX = 0, m_offsetY = 0;           // live drag offset
    bool m_dragging = false;
    bool m_clickPending = false;
};
