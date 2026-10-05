#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Orthographic camera, always follows the current pivot planet.
// Fixed relativeTo=Player, position=(0,0).

namespace adofai {

class Camera {
public:
    Camera();

    void setZoom(float zoom);
    void setAspect(float width, float height);
    void setTarget(double x, double y);

    glm::mat4 viewProj() const;
    glm::mat4 proj()    const { return m_proj; }
    glm::mat4 view()    const { return m_view; }

    float zoom() const { return m_zoom; }
    double targetX() const { return m_targetX; }
    double targetY() const { return m_targetY; }
    void frustumBounds(float& left, float& right, float& bottom, float& top) const;

private:
    float m_zoom   = 100.0f;
    float m_aspect = 16.0f / 9.0f;
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
