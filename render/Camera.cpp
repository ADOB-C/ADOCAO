#include "Camera.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>


namespace adofai {

namespace {
// 两套"100"的对照（docs/camera.md §7.3 / §12.2）：
//   * 本类：`halfH = 6/(zoom/100)` → 可见高度 12 砖 @ zoom=100；
//   * Re_ADOJAS：`baseFrustumSize = 5.47` → 可见高度 5.47 砖 @ adoZoom=100。
// 谱面 zoom 想表达的可见高度 = 5.47 * adoZoom/100，所以：
//   本类 zoom = 100 * 12 / (5.47 * adoZoom/100) = 21937.8427... / adoZoom
// 两个"100"都不动：`--capture-zoom` 与滚轮仍然是本类的放大倍数（既有像素基线靠它）。
constexpr double kAdoZoomNumerator = 120000.0 / 5.47;
}  // namespace

Camera::Camera() {
    m_view = glm::lookAt(
        glm::vec3(0.0f, 0.0f, 10.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f));
    updateProj();
}

void Camera::setZoom(float zoom) {
    if (m_zoom == zoom) return;
    m_zoom = zoom;
    m_projDirty = true;
}

void Camera::setAdoZoom(float adoZoom) {
    // clamp：谱面里出现 0/负数时别把投影做成 inf（视觉上"整屏空白"比不缩放更糟）
    // 只兜底 0/负数（会除零/投影 inf）。Re_ADOJAS 的 setZoom 不做上下限夹取；
    // 谱面/用户给多大就是多大（极端值仍是有限 float：21937.8/1e9 = 2.2e-5）。
    double z = (double)adoZoom;
    if (!(z > 0.0)) z = 100.0;
    setZoom((float)(kAdoZoomNumerator / z));
}

void Camera::setRotation(float degrees) {
    if (m_rotationDeg == degrees) return;
    m_rotationDeg = degrees;
    m_projDirty = true;
}

void Camera::setAspect(float width, float height) {
    float newAspect = width / height;
    if (m_aspect == newAspect) return;
    m_aspect = newAspect;
    m_projDirty = true;
}

void Camera::setTarget(double x, double y) {
    m_targetX = x;
    m_targetY = y;
}

void Camera::updateProj() const {
    m_halfH = 6.0f / (m_zoom / 100.0f);
    m_halfW = m_halfH * m_aspect;
    // 旋转并进投影：顶点是相机相对坐标，屏幕 = R(-θ)·rel，所以 proj = ortho · R(-θ)。
    // 绕 z 旋转不动 z，深度/实例排序不受影响。
    const float rad = m_rotationDeg * 3.14159265358979323846f / 180.0f;
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), -rad, glm::vec3(0.0f, 0.0f, 1.0f));
    m_proj = glm::ortho(-m_halfW, m_halfW, -m_halfH, m_halfH, 0.1f, 200.0f) * rot;
    m_projView = m_proj * m_view;
    m_projDirty = false;
}

glm::mat4 Camera::viewProj() const {
    if (m_projDirty) updateProj();
    return m_projView;
}

void Camera::frustumBounds(double& left, double& right, double& bottom, double& top) const {
    if (m_projDirty) updateProj();
    // 旋转后可见区是**旋转矩形**；剔除要的是它的世界 AABB（超集）。用超集只会多剔一点，
    // 不会漏（漏了就是边缘缺砖）。θ=0 时退化成原来的轴对齐框，逐位不变。
    const double rad = (double)m_rotationDeg * 3.14159265358979323846 / 180.0;
    const double c = std::fabs(std::cos(rad));
    const double s = std::fabs(std::sin(rad));
    const double hw = c * (double)m_halfW + s * (double)m_halfH;
    const double hh = s * (double)m_halfW + c * (double)m_halfH;
    left   = m_targetX - hw;
    right  = m_targetX + hw;
    bottom = m_targetY - hh;
    top    = m_targetY + hh;
}

}  // namespace adofai
