#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Orthographic camera, always follows the current pivot planet.
// Fixed relativeTo=Player, position=(0,0).

namespace adofai {

class Camera {
public:
    Camera();

    // 本类的 `zoom` 是**放大倍数**：越大越近（100 = 视野高 12 砖）。
    // 谱面给的 zoom（`settings.zoom` / `MoveCamera.zoom`）是 ADOFAI 口径：**越大看得越远**
    // （Re_ADOJAS：`camera.zoom = 100/adoZoom`）。两者方向相反，谱面的值一律走 setAdoZoom()。
    void setZoom(float zoom);
    // ADOFAI 口径的缩放（100 = 默认；数值越大看得越远）。换算见 .cpp 的常数说明。
    void setAdoZoom(float adoZoom);
    void setAspect(float width, float height);
    void setTarget(double x, double y);
    // 相机绕视线轴旋转（度）。正值 = 相机逆时针 → 画面里的世界顺时针转
    // （与 Re_ADOJAS 的 `camera.rotation.z = deg*π/180` 同一个符号；docs/camera.md §7.3）。
    // 顶点已经是"相机相对"的，所以旋转直接并进 proj（`ortho * R(-θ)`）。
    void setRotation(float degrees);

    glm::mat4 viewProj() const;

    float zoom() const { return m_zoom; }
    // 一砖在屏幕上占多少像素（视野高 = 12 砖 × 100/zoom）——拖尾的 sagitta 判据要用
    float pixelsPerTile(int framebufferHeight) const {
        return framebufferHeight * m_zoom / 1200.0f;
    }
    float rotation() const { return m_rotationDeg; }
    double targetX() const { return m_targetX; }
    double targetY() const { return m_targetY; }
    // **double**：视锥边界是世界坐标。用 float 会在 x≈6.8e7（primer）上被量化到 8 砖，
    // 剔除边界跟着错（半宽半高本身很小，double 里算没有代价）。
    void frustumBounds(double& left, double& right, double& bottom, double& top) const;

private:
    float m_zoom   = 100.0f;
    float m_aspect = 16.0f / 9.0f;
    float m_rotationDeg = 0.0f;
    double m_targetX = 0.0;
    double m_targetY = 0.0;

    // Lazy-evaluated cached values (mutable for const-method access)
    mutable float m_halfH = 6.0f;
    mutable float m_halfW = 6.0f * 16.0f / 9.0f;
    mutable glm::mat4 m_proj = glm::mat4(1.0f);
    mutable glm::mat4 m_projView = glm::mat4(1.0f);
    mutable bool m_projDirty = true;

    glm::mat4 m_view = glm::mat4(1.0f);  // constant after construction

    void updateProj() const;
};

}  // namespace adofai
