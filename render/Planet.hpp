#pragma once

#include "glad/gl_core.hpp"
#include <glm/glm.hpp>


namespace adofai {

class PlanetTrail;
class Shader;
class Camera;

class Planet {
public:
    // **double**：世界坐标到 6.84e7（primer）时 float 的 ulp 是 8 砖，存 float 会让行星与
    // 砖块各自量化到不同格点（2026-10-08 用户实测"行星也不对了"）。相机相对化在 draw() 里做。
    glm::dvec3 position{0.0};
    glm::vec3 color{1.0f};
    float radius = 0.25f;

    PlanetTrail* trail = nullptr;

    Planet() = default;
    Planet(const glm::vec3& color, bool showTrail, float planetRadius = 0.22f);
    ~Planet();

    Planet(const Planet&) = delete;
    Planet& operator=(const Planet&) = delete;

    void setTrailPoints(const double* xy, int count, int maxExpected);

    void buildGPU();
    void draw(Shader& shader, const Camera& camera, double camX, double camY) const;
    bool gpuBuilt() const { return m_vao != 0; }

private:
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    unsigned m_indexCount = 0;

    void destroyGPU();
};

}  // namespace adofai
