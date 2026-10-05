#pragma once

#include "glad/gl_core.hpp"
#include <glm/glm.hpp>


namespace adofai {

class PlanetTrail;
class Shader;
class Camera;

class Planet {
public:
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float radius = 0.25f;

    PlanetTrail* trail = nullptr;

    Planet() = default;
    Planet(const glm::vec3& color, bool showTrail, float planetRadius = 0.25f);
    ~Planet();

    Planet(const Planet&) = delete;
    Planet& operator=(const Planet&) = delete;
    Planet(Planet&&) noexcept;
    Planet& operator=(Planet&&) noexcept;

    void setTrailPoints(const double* xy, int count, int maxExpected);
    void clearTrail();

    bool buildGPU();
    void draw(Shader& shader, const Camera& camera, double camX, double camY) const;
    bool gpuBuilt() const { return m_vao != 0; }
    GLuint vao() const { return m_vao; }
    unsigned indexCount() const { return m_indexCount; }

private:
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    unsigned m_indexCount = 0;

    void destroyGPU();
};

}  // namespace adofai
