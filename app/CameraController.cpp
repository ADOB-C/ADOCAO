#include "CameraController.hpp"

#include "render/Camera.hpp"

#include <algorithm>
#include <cmath>

LetterboxedViewport computeLetterbox(int fbW, int fbH, float targetAspect) {
    float fbAspect = (float)fbW / (float)fbH;
    LetterboxedViewport vp;
    if (targetAspect > fbAspect) {
        vp.w = fbW; vp.h = (int)(fbW / targetAspect); vp.x = 0; vp.y = (fbH - vp.h) / 2;
    } else {
        vp.h = fbH; vp.w = (int)(fbH * targetAspect); vp.x = (fbW - vp.w) / 2; vp.y = 0;
    }
    return vp;
}

// Half viewport size in world units at the current zoom. The world-space
// window is 12 units tall at zoom 100 (6 above / 6 below the camera target).
static double halfViewHeight(float zoom) {
    return 6.0 / (zoom / 100.0);
}

// World units per pixel along each axis inside the letterboxed viewport.
static void pixelToWorldScale(double halfH, const LetterboxedViewport& vp,
                              double& pxToWorldX, double& pxToWorldY) {
    double halfW = halfH * (double)vp.w / (double)vp.h;
    pxToWorldX = (2.0 * halfW) / (double)vp.w;
    pxToWorldY = (2.0 * halfH) / (double)vp.h;
}

void CameraController::snapTo(double worldX, double worldY) {
    m_camera->setTarget(worldX, worldY);
    m_baseTargetX = worldX;
    m_baseTargetY = worldY;
    m_offsetX = 0;
    m_offsetY = 0;
}

void CameraController::onMouseButton(bool pressed) {
    if (pressed) {
        m_dragging = true;
        m_dragStartX = m_cursorX;
        m_dragStartY = m_cursorY;
    } else {
        double dx = m_cursorX - m_dragStartX, dy = m_cursorY - m_dragStartY;
        m_dragging = false;
        // Release commits any pending drag offset into the base target.
        m_baseTargetX += m_offsetX;
        m_baseTargetY += m_offsetY;
        m_offsetX = 0;
        m_offsetY = 0;
        if (dx * dx + dy * dy < 25.0) m_clickPending = true;
    }
}

void CameraController::onCursorPos(double x, double y) {
    m_cursorX = x;
    m_cursorY = y;
}

void CameraController::onScroll(double yOffset) {
    float minZoom = ADOCAO_MIN_ZOOM, maxZoom = 1000.0f;
    float z = m_camera->zoom() * (1.0f + (float)yOffset * 0.1f);
    if (z < minZoom) z = minZoom;
    if (z > maxZoom) z = maxZoom;
    m_camera->setZoom(z);
}

bool CameraController::consumeClick() {
    bool click = m_clickPending;
    m_clickPending = false;
    return click;
}

void CameraController::update(int fbW, int fbH, float targetAspect, bool panEnabled) {
    if (!panEnabled) return;
    if (m_dragging) {
        LetterboxedViewport vp = computeLetterbox(fbW, fbH, targetAspect);
        if (vp.w > 0 && vp.h > 0) {
            double pxToWorldX, pxToWorldY;
            pixelToWorldScale(halfViewHeight(m_camera->zoom()), vp, pxToWorldX, pxToWorldY);
            m_offsetX = -(m_cursorX - m_dragStartX) * pxToWorldX;
            m_offsetY =  (m_cursorY - m_dragStartY) * pxToWorldY;
        }
    }
    m_camera->setTarget(m_baseTargetX + m_offsetX, m_baseTargetY + m_offsetY);
}

glm::dvec2 CameraController::screenToWorld(double x, double y, int fbW, int fbH,
                                           float targetAspect) const {
    LetterboxedViewport vp = computeLetterbox(fbW, fbH, targetAspect);
    double halfH = halfViewHeight(m_camera->zoom());
    double pxToWorldX, pxToWorldY;
    pixelToWorldScale(halfH, vp, pxToWorldX, pxToWorldY);
    double halfW = halfH * (double)vp.w / (double)vp.h;
    double worldX = m_camera->targetX() + (x - vp.x) * pxToWorldX - halfW;
    double worldY = m_camera->targetY() - (y - vp.y) * pxToWorldY + halfH;
    return {worldX, worldY};
}
