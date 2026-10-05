#include "Camera.hpp"


namespace adofai {

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
    m_proj = glm::ortho(-m_halfW, m_halfW, -m_halfH, m_halfH, 0.1f, 200.0f);
    m_projView = m_proj * m_view;
    m_projDirty = false;
}

glm::mat4 Camera::viewProj() const {
    if (m_projDirty) updateProj();
    return m_projView;
}

void Camera::frustumBounds(float& left, float& right, float& bottom, float& top) const {
    if (m_projDirty) updateProj();
    left   = (float)(m_targetX - (double)m_halfW);
    right  = (float)(m_targetX + (double)m_halfW);
    bottom = (float)(m_targetY - (double)m_halfH);
    top    = (float)(m_targetY + (double)m_halfH);
}

}  // namespace adofai
