#pragma once

#include "glad/gl_core.hpp"
#include <glm/glm.hpp>
#include <vector>


namespace adofai {

class Shader;
class Camera;

class PlanetTrail {
public:
    PlanetTrail(const glm::vec3& color, float planetRadius);
    ~PlanetTrail();

    PlanetTrail(const PlanetTrail&) = delete;
    PlanetTrail& operator=(const PlanetTrail&) = delete;
    PlanetTrail(PlanetTrail&&) noexcept;
    PlanetTrail& operator=(PlanetTrail&&) noexcept;

    void setPoints(const double* xy, int count, int maxExpected);
    void draw(Shader& shader, const Camera& camera, double camX, double camY);
    void clear();

private:
    struct Point { glm::dvec2 pos; float time; };
    std::vector<Point> m_points;
    int m_head = 0;
    int m_count = 0;

    std::vector<float> m_verts;
    std::vector<unsigned> m_indices;

    glm::dvec2 m_center{0.0};

    int m_maxPoints = 0;
    float m_planetRadius;
    glm::vec3 m_color;

    mutable GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    mutable int m_gpuMaxPoints = 0;  // GL buffers sized for this many ring points
    mutable unsigned m_vertexCount = 0, m_indexCount = 0;
    mutable bool m_dirty = false;

    Point& ringAt(int i) { return m_points[(m_head + i) % m_maxPoints]; }
    const Point& ringAt(int i) const { return m_points[(m_head + i) % m_maxPoints]; }

    static glm::dvec2 catmullRom(const glm::dvec2& p0, const glm::dvec2& p1,
                                const glm::dvec2& p2, const glm::dvec2& p3, float t);
    static glm::dvec2 catmullRomTangent(const glm::dvec2& p0, const glm::dvec2& p1,
                                       const glm::dvec2& p2, const glm::dvec2& p3, float t);

    void rebuildGeometry();
    void ensureGPUResources() const;
};

}  // namespace adofai
