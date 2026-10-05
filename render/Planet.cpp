#include "Planet.hpp"
#include "render/PlanetTrail.hpp"
#include "render/Shader.hpp"
#include "render/Camera.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>


namespace adofai {

Planet::Planet(const glm::vec3& color, bool showTrail, float r)
    : color(color), radius(r) {
    if (showTrail)
        trail = new PlanetTrail(color, radius);
}

Planet::~Planet() {
    destroyGPU();
    delete trail;
}

Planet::Planet(Planet&& o) noexcept
    : position(o.position), color(o.color), radius(o.radius), trail(o.trail),
      m_vao(o.m_vao), m_vbo(o.m_vbo), m_ebo(o.m_ebo), m_indexCount(o.m_indexCount) {
    o.trail = nullptr;
    o.m_vao = o.m_vbo = o.m_ebo = 0;
    o.m_indexCount = 0;
}

Planet& Planet::operator=(Planet&& o) noexcept {
    if (this != &o) {
        destroyGPU();
        delete trail;
        position = o.position; color = o.color; radius = o.radius;
        trail = o.trail; m_vao = o.m_vao; m_vbo = o.m_vbo; m_ebo = o.m_ebo;
        m_indexCount = o.m_indexCount;
        o.trail = nullptr;
        o.m_vao = o.m_vbo = o.m_ebo = 0;
        o.m_indexCount = 0;
    }
    return *this;
}

void Planet::setTrailPoints(const double* xy, int count, int maxExpected) {
    if (trail)
        trail->setPoints(xy, count, maxExpected);
}

void Planet::clearTrail() {
    if (trail)
        trail->clear();
}

void Planet::destroyGPU() {
    if (m_ebo) { glDeleteBuffers(1, &m_ebo); m_ebo = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
}

bool Planet::buildGPU() {
    destroyGPU();

    constexpr int rings = 24, sectors = 24;
    std::vector<float> verts;
    std::vector<unsigned> indices;

    // Generate UV sphere vertices (no color — color comes from uniform)
    for (int r = 0; r <= rings; r++) {
        float phi = 3.14159265f * (float)r / (float)rings;
        float sinPhi = std::sin(phi), cosPhi = std::cos(phi);

        for (int s = 0; s <= sectors; s++) {
            float theta = 2.0f * 3.14159265f * (float)s / (float)sectors;
            float sinTheta = std::sin(theta), cosTheta = std::cos(theta);

            float x = cosTheta * sinPhi;
            float y = cosPhi;
            float z = sinTheta * sinPhi;
            verts.insert(verts.end(), {x * radius, y * radius, z * radius});
        }
    }

    // Generate triangle strip indices
    for (int r = 0; r < rings; r++) {
        for (int s = 0; s <= sectors; s++) {
            unsigned cur = r * (sectors + 1) + s;
            unsigned next = (r + 1) * (sectors + 1) + s;
            indices.push_back(cur);
            indices.push_back(next);
        }
        // Degenerate to restart strip
        if (r < rings - 1) {
            unsigned nextStart = (r + 1) * (sectors + 1) + sectors;
            unsigned nextNext = (r + 2) * (sectors + 1);
            indices.push_back(nextStart);
            indices.push_back(nextNext);
        }
    }

    m_indexCount = (unsigned)indices.size();

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    glGenBuffers(1, &m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned), indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);
    return true;
}

void Planet::draw(Shader& shader, const Camera& camera, double camX, double camY) const {
    if (!m_vao) return;

    glm::vec3 relPos((float)((double)position.x - camX), (float)((double)position.y - camY), position.z);
    auto model = glm::translate(glm::mat4(1.0f), relPos);
    auto mvp = camera.viewProj() * model;

    shader.use();
    shader.setMat4("uMVP", glm::value_ptr(mvp));
    shader.setVec4("uColor", color.x, color.y, color.z, 1.0f);

    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLE_STRIP, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

}  // namespace adofai
