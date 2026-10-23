#include "CameraController.hpp"

#include "core/util/Env.hpp"
#include "core/util/Logger.hpp"
#include "render/Camera.hpp"

#include <algorithm>
#include <cmath>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

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

// Screen-space (camera-relative) -> world offset, undoing the camera rotation.
// 相机的屏幕变换是 `screen = R(-θ)·rel`（见 render/Camera::updateProj），所以
// `rel = R(θ)·screen`。θ = 0 时逐位退化成原来的轴对齐算法。
static void unrotate(const Camera& cam, double& x, double& y) {
    const float deg = cam.rotation();
    if (deg == 0.0f) return;                 // 常见路径：不做任何浮点运算
    const double rad = (double)deg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);
    const double nx = c * x - s * y;
    const double ny = s * x + c * y;
    x = nx;
    y = ny;
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
    // 低成本的生命周期日志：先确认"按/松"到底有没有到达控制器（相机不动时先排除这条）。
    if (env::flag("ADOCAO_DRAG_DEBUG")) {
        LOG_I("mouse %s at (%.1f,%.1f)", pressed ? "down" : "up", m_cursorX, m_cursorY);
    }
    if (pressed) {
        m_dragging = true;
        m_dragStartX = m_cursorX;
        m_dragStartY = m_cursorY;
        m_dragFrames = 0;
        m_prevTargetX = m_camera ? m_camera->targetX() : 0.0;
        m_prevTargetY = m_camera ? m_camera->targetY() : 0.0;
        // **拖动前先把输入日志打出来**：`m_cursorX/Y` 是"最后一次光标回调"的值，
        // 如果它还是 (0,0)（窗口刚建好、鼠标没动过就按下），dragStart 就是错的 ——
        // 第一次移动会算出一个巨大的 delta，镜头直接"传送"。这条日志就是为了看清这一点。
        if (env::flag("ADOCAO_DRAG_DEBUG")) {
            LOG_I("drag-start: cursor=(%.2f,%.2f) base=(%.3f,%.3f) zoom=%.2f seen=%d win=%dx%d",
                  m_cursorX, m_cursorY, m_baseTargetX, m_baseTargetY,
                  m_camera ? (double)m_camera->zoom() : 0.0, (int)m_cursorSeen, m_winW, m_winH);
        }
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
    m_cursorSeen = true;
}

void CameraController::onScroll(double yOffset) {
    // 只记下来：**缩放要以光标为中心**（光标下的世界点保持不动），
    // 而那需要视口尺寸，只有 update() 有。学自 ADOCAO-E 的编辑器（它先做了这件事）。
    m_pendingScroll += yOffset;
}

bool CameraController::consumeClick() {
    bool click = m_clickPending;
    m_clickPending = false;
    return click;
}

void CameraController::update(int fbW, int fbH, float targetAspect, bool panEnabled,
                              int winW, int winH) {
    m_lastFbW = fbW; m_lastFbH = fbH; m_lastAspect = targetAspect;
    if (winW > 0 && winH > 0) { m_winW = winW; m_winH = winH; }
    // 光标从没进过窗口 / 尺寸还没拿到 → **不做锚定**（退回以中心缩放）。
    // 否则锚点会落在像素 (0,0)，也就是"缩放中心在屏幕左上角"。
    const bool canAnchor = m_cursorSeen && m_winW > 0 && m_winH > 0 && fbW > 0 && fbH > 0;
    // ── 滚轮：以光标为中心缩放（光标下的世界点保持不动）──
    if (m_pendingScroll != 0.0 && canAnchor) {
        // 光标是**窗口点**，换成帧缓冲像素再算（Retina / 全屏下两者不等）
        const double cx = cursorFbX(fbW), cy = cursorFbY(fbH);
        const glm::dvec2 before = screenToWorld(cx, cy, fbW, fbH, targetAspect);
        // 指数步进（触控板的小数增量也顺滑）；倍率与 ADOCAO-E 一致（1.15/格）。
        // 上限从 1000 放宽：谱面 zoom 是 ADOFAI 口径、会映射成较大的放大倍数。
        const float minZoom = ADOCAO_MIN_ZOOM, maxZoom = 100000.0f;
        const float z0 = m_camera->zoom();
        float z = m_camera->zoom() * (float)std::pow(1.15, m_pendingScroll);
        z = std::min(std::max(z, minZoom), maxZoom);
        m_camera->setZoom(z);
        const glm::dvec2 after = screenToWorld(cx, cy, fbW, fbH, targetAspect);
        if (env::flag("ADOCAO_ZOOM_DEBUG")) {
            LOG_I("zoom: cursor_pts=(%.1f,%.1f) fb=(%.0f,%.0f) win=%dx%d fb=%dx%d zoom %.1f→%.1f 世界点 (%.4f,%.4f)→(%.4f,%.4f) target (%.4f,%.4f)",
                  m_cursorX, m_cursorY, cx, cy, m_winW, m_winH, fbW, fbH, (double)z0, (double)m_camera->zoom(),
                  before.x, before.y, after.x, after.y, m_baseTargetX, m_baseTargetY);
        }
        m_baseTargetX += before.x - after.x;
        m_baseTargetY += before.y - after.y;
        m_offsetX = m_offsetY = 0.0;
        m_pendingScroll = 0.0;
    } else if (m_pendingScroll != 0.0) {
        // 退化路径：仍然缩放，只是以中心为锚点。
        const float minZoom = ADOCAO_MIN_ZOOM, maxZoom = 100000.0f;
        float z = m_camera->zoom() * (float)std::pow(1.15, m_pendingScroll);
        m_camera->setZoom(std::min(std::max(z, minZoom), maxZoom));
        m_pendingScroll = 0.0;
    }
    if (!panEnabled) return;
    if (m_dragging) {
        LetterboxedViewport vp = computeLetterbox(fbW, fbH, targetAspect);
        if (vp.w > 0 && vp.h > 0) {
            double pxToWorldX, pxToWorldY;
            pixelToWorldScale(halfViewHeight(m_camera->zoom()), vp, pxToWorldX, pxToWorldY);
            // 光标增量是**窗口点**，而 pxToWorld 是**帧缓冲像素**口径 —— Retina 上差 content scale。
            // 这里先按帧缓冲换算（与缩放/拾取同一条口径）。
            const double sx = m_winW > 0 ? (double)fbW / (double)m_winW : 1.0;
            const double sy = m_winH > 0 ? (double)fbH / (double)m_winH : 1.0;
            const double mdx = (m_cursorX - m_dragStartX) * sx;
            const double mdy = (m_cursorY - m_dragStartY) * sy;
            m_offsetX = -mdx * pxToWorldX;
            m_offsetY =  mdy * pxToWorldY;
            unrotate(*m_camera, m_offsetX, m_offsetY);      // 旋转视角下拖动方向也跟着转
            // **默认就记**：光标这一帧几乎没动、镜头却移了 >1 砖 —— 那就是"传送"。
            // （很少见，所以不必藏到 env 后面；每帧细节仍走 ADOCAO_DRAG_DEBUG。）
            {
                const double tx0 = m_baseTargetX + m_offsetX, ty0 = m_baseTargetY + m_offsetY;
                const double camDx0 = tx0 - m_prevTargetX, camDy0 = ty0 - m_prevTargetY;
                const double mousePts = std::hypot(m_cursorX - m_dragStartX, m_cursorY - m_dragStartY);
                if (mousePts < 4.0 && camDx0 * camDx0 + camDy0 * camDy0 > 1.0) {
                    LOG_W("drag-jump: 光标只移了 %.2f 点，镜头却移了 (%.3f,%.3f) 砖 —— "
                          "cursor=(%.1f,%.1f) start=(%.1f,%.1f) zoom=%.1f win=%dx%d fb=%dx%d",
                          mousePts, camDx0, camDy0, m_cursorX, m_cursorY, m_dragStartX, m_dragStartY,
                          (double)m_camera->zoom(), m_winW, m_winH, fbW, fbH);
                }
            }
            if (env::flag("ADOCAO_DRAG_DEBUG")) {
                const double tx = m_baseTargetX + m_offsetX, ty = m_baseTargetY + m_offsetY;
                const double camDx = tx - m_prevTargetX, camDy = ty - m_prevTargetY;
                const double mousePx = std::hypot(m_cursorX - m_dragStartX, m_cursorY - m_dragStartY);
                // "鼠标这一帧没怎么动，镜头却大幅移动" = 传送的典型征兆
                if (m_dragFrames < 40 || (camDx * camDx + camDy * camDy > 1.0 && mousePx < 4.0)) {
                    LOG_I("drag[%d]: cursor=(%.1f,%.1f) d=%.2fpt  offset=(%.3f,%.3f) target=(%.3f,%.3f) Δcam=(%.3f,%.3f) zoom=%.1f",
                          m_dragFrames, m_cursorX, m_cursorY, mousePx, m_offsetX, m_offsetY,
                          tx, ty, camDx, camDy, (double)m_camera->zoom());
                }
                ++m_dragFrames;
                m_prevTargetX = tx;
                m_prevTargetY = ty;
            }
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
    double relX = (x - vp.x) * pxToWorldX - halfW;
    double relY = -(y - vp.y) * pxToWorldY + halfH;
    unrotate(*m_camera, relX, relY);
    return {m_camera->targetX() + relX, m_camera->targetY() + relY};
}
