#include "PlanetTrail.hpp"
#include "render/Shader.hpp"
#include "render/Camera.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>


namespace adofai {

PlanetTrail::PlanetTrail(const glm::vec3& color, float planetRadius)
    : m_planetRadius(planetRadius), m_color(color) {
    m_points.resize(m_maxPoints);
}

PlanetTrail::~PlanetTrail() {
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

PlanetTrail::PlanetTrail(PlanetTrail&& o) noexcept
    : m_points(std::move(o.m_points)), m_head(o.m_head), m_count(o.m_count),
      m_verts(std::move(o.m_verts)), m_indices(std::move(o.m_indices)),
      m_center(o.m_center), m_maxPoints(o.m_maxPoints),
      m_planetRadius(o.m_planetRadius),
      m_color(o.m_color), m_vao(o.m_vao), m_vbo(o.m_vbo), m_ebo(o.m_ebo),
      m_gpuMaxPoints(o.m_gpuMaxPoints),
      m_vertexCount(o.m_vertexCount), m_indexCount(o.m_indexCount), m_dirty(o.m_dirty) {
    o.m_vao = o.m_vbo = o.m_ebo = 0;
    o.m_gpuMaxPoints = 0;
    o.m_vertexCount = o.m_indexCount = 0;
}

PlanetTrail& PlanetTrail::operator=(PlanetTrail&& o) noexcept {
    if (this != &o) {
        if (m_ebo) glDeleteBuffers(1, &m_ebo);
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        m_points = std::move(o.m_points); m_head = o.m_head; m_count = o.m_count;
        m_verts = std::move(o.m_verts); m_indices = std::move(o.m_indices);
        m_center = o.m_center; m_maxPoints = o.m_maxPoints;
        m_planetRadius = o.m_planetRadius;
        m_color = o.m_color;
        m_vao = o.m_vao; m_vbo = o.m_vbo; m_ebo = o.m_ebo;
        m_gpuMaxPoints = o.m_gpuMaxPoints;
        m_vertexCount = o.m_vertexCount; m_indexCount = o.m_indexCount;
        m_dirty = o.m_dirty;
        o.m_vao = o.m_vbo = o.m_ebo = 0;
        o.m_gpuMaxPoints = 0;
        o.m_vertexCount = o.m_indexCount = 0;
    }
    return *this;
}

glm::dvec2 PlanetTrail::catmullRom(const glm::dvec2& p0, const glm::dvec2& p1,
                                    const glm::dvec2& p2, const glm::dvec2& p3, float t) {
    double t2 = t * t, t3 = t2 * t;
    double c0 = -0.5 * t3 + t2 - 0.5 * t;
    double c1 =  1.5 * t3 - 2.5 * t2 + 1.0;
    double c2 = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    double c3 =  0.5 * t3 - 0.5 * t2;
    return c0 * p0 + c1 * p1 + c2 * p2 + c3 * p3;
}

glm::dvec2 PlanetTrail::catmullRomTangent(const glm::dvec2& p0, const glm::dvec2& p1,
                                           const glm::dvec2& p2, const glm::dvec2& p3, float t) {
    double t2 = t * t;
    double c0 = -1.5 * t2 + 2.0 * t - 0.5;
    double c1 =  4.5 * t2 - 5.0 * t;
    double c2 = -4.5 * t2 + 4.0 * t + 0.5;
    double c3 =  1.5 * t2 - 1.0 * t;
    return c0 * p0 + c1 * p1 + c2 * p2 + c3 * p3;
}

void PlanetTrail::clear() {
    m_head = 0;
    m_count = 0;
    m_dirty = true;
}

void PlanetTrail::setPoints(const double* xy, int count, int maxExpected) {
    if (count < 2) { clear(); return; }
    int needed = std::max(count, maxExpected);
    if (needed > m_maxPoints) { m_maxPoints = needed; m_points.resize(m_maxPoints); }
    int skip = 0;
    if (count > m_maxPoints) { skip = count - m_maxPoints; count = m_maxPoints; }
    m_head = 0; m_count = count;
    for (int i = 0; i < count; i++) {
        m_points[i].pos = {xy[(skip + i)*2], xy[(skip + i)*2 + 1]};
        m_points[i].time = 0.0f;
    }
    ensureGPUResources();
    int n = m_count;
    m_center = (ringAt(0).pos + ringAt(n-1).pos) * 0.5;
    const int segsPerPoint = 4;
    int totalSegments = (n-1) * segsPerPoint;
    int vertCount = (totalSegments+1)*2, idxCount = totalSegments*6;
    m_verts.resize(vertCount*3); m_indices.resize(idxCount);
    double maxWidth = (double)(m_planetRadius * 2.0f);
    int vi = 0;
    for (int seg = 0; seg <= totalSegments; seg++) {
        double globalT = (double)seg / (double)totalSegments;
        double rawIdx = globalT * (double)(n - 1);
        int i = (int)rawIdx; double localT = rawIdx - (double)i;
        glm::dvec2 p0=ringAt(std::max(0,i-1)).pos, p1=ringAt(i).pos;
        glm::dvec2 p2=ringAt(std::min(n-1,i+1)).pos, p3=ringAt(std::min(n-1,i+2)).pos;
        glm::dvec2 pt=catmullRom(p0,p1,p2,p3,(float)localT);
        glm::dvec2 tangent=catmullRomTangent(p0,p1,p2,p3,(float)localT);
        double len=std::sqrt(tangent.x*tangent.x+tangent.y*tangent.y);
        glm::dvec2 normal=(len>0.001)?glm::dvec2(-tangent.y/len,tangent.x/len):glm::dvec2(0,1);
        double width=maxWidth*globalT;
        m_verts[vi*3]=(float)(pt.x-m_center.x-normal.x*width*0.5);
        m_verts[vi*3+1]=(float)(pt.y-m_center.y-normal.y*width*0.5);m_verts[vi*3+2]=0;vi++;
        m_verts[vi*3]=(float)(pt.x-m_center.x+normal.x*width*0.5);
        m_verts[vi*3+1]=(float)(pt.y-m_center.y+normal.y*width*0.5);m_verts[vi*3+2]=0;vi++;
    }
    int ii = 0;
    for (int seg = 0; seg < totalSegments; seg++) {
        unsigned base = seg*2;
        m_indices[ii++]=base; m_indices[ii++]=base+1; m_indices[ii++]=base+2;
        m_indices[ii++]=base+1; m_indices[ii++]=base+3; m_indices[ii++]=base+2;
    }
    m_vertexCount=vertCount; m_indexCount=idxCount; m_dirty=false;
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER,m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER,0,m_verts.size()*sizeof(float),m_verts.data());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,m_ebo);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,0,m_indices.size()*sizeof(unsigned),m_indices.data());
    glBindVertexArray(0);
}

void PlanetTrail::ensureGPUResources() const {
    if (m_maxPoints < 2) return;
    if (m_vao == 0) glGenVertexArrays(1, &m_vao);
    if (m_maxPoints <= m_gpuMaxPoints) return;   // already sized for this ring

    // (Re)allocate VBO/EBO for the current ring capacity. The sample rate is
    // adaptive, so m_maxPoints can grow between frames — buffers must follow,
    // otherwise glBufferSubData would write past the allocation.
    constexpr int segsPerPoint = 4;
    int maxSegments = (m_maxPoints - 1) * segsPerPoint;
    int maxVerts = (maxSegments + 1) * 2;
    int maxIndices = maxSegments * 6;
    glBindVertexArray(m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    glGenBuffers(1, &m_vbo); glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)maxVerts * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    glGenBuffers(1, &m_ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)maxIndices * sizeof(unsigned), nullptr, GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
    m_gpuMaxPoints = m_maxPoints;
}

void PlanetTrail::rebuildGeometry() {
    ensureGPUResources();
    int n = m_count;
    if (n < 2) { m_vertexCount = 0; m_indexCount = 0; return; }
    m_center = (ringAt(0).pos + ringAt(n-1).pos) * 0.5;
    const int segsPerPoint = 4;
    int totalSegments = (n-1)*segsPerPoint, vertCount = (totalSegments+1)*2, idxCount = totalSegments*6;
    m_verts.resize(vertCount*3); m_indices.resize(idxCount);
    double maxWidth = (double)(m_planetRadius * 2.0f);
    int vi = 0;
    for (int seg = 0; seg <= totalSegments; seg++) {
        double globalT=(double)seg/(double)totalSegments, rawIdx=globalT*(double)(n-1);
        int i=(int)rawIdx; double localT=rawIdx-(double)i;
        glm::dvec2 p0=ringAt(std::max(0,i-1)).pos, p1=ringAt(i).pos;
        glm::dvec2 p2=ringAt(std::min(n-1,i+1)).pos, p3=ringAt(std::min(n-1,i+2)).pos;
        glm::dvec2 pt=catmullRom(p0,p1,p2,p3,(float)localT);
        glm::dvec2 tangent=catmullRomTangent(p0,p1,p2,p3,(float)localT);
        double len=std::sqrt(tangent.x*tangent.x+tangent.y*tangent.y);
        glm::dvec2 normal=(len>0.001)?glm::dvec2(-tangent.y/len,tangent.x/len):glm::dvec2(0,1);
        double width=maxWidth*globalT;
        m_verts[vi*3]=(float)(pt.x-m_center.x-normal.x*width*0.5);
        m_verts[vi*3+1]=(float)(pt.y-m_center.y-normal.y*width*0.5);m_verts[vi*3+2]=0;vi++;
        m_verts[vi*3]=(float)(pt.x-m_center.x+normal.x*width*0.5);
        m_verts[vi*3+1]=(float)(pt.y-m_center.y+normal.y*width*0.5);m_verts[vi*3+2]=0;vi++;
    }
    int ii = 0;
    for (int seg = 0; seg < totalSegments; seg++) {
        unsigned base = seg*2;
        m_indices[ii++]=base; m_indices[ii++]=base+1; m_indices[ii++]=base+2;
        m_indices[ii++]=base+1; m_indices[ii++]=base+3; m_indices[ii++]=base+2;
    }
    m_vertexCount=vertCount; m_indexCount=idxCount;
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER,m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER,0,m_verts.size()*sizeof(float),m_verts.data());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,m_ebo);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,0,m_indices.size()*sizeof(unsigned),m_indices.data());
    glBindVertexArray(0);
    m_dirty=false;
}

void PlanetTrail::draw(Shader& shader, const Camera& camera, double camX, double camY) {
    if (m_count < 2) return;
    if (m_dirty) rebuildGeometry();
    if (m_indexCount == 0) return;
    // Camera-relative offset in double precision (like tiles)
    glm::dvec2 offset = m_center - glm::dvec2(camX, camY);
    auto model = glm::translate(glm::mat4(1.0f),
        glm::vec3((float)offset.x, (float)offset.y, 0));
    auto mvp = camera.viewProj() * model;
    shader.use();
    shader.setMat4("uMVP", glm::value_ptr(mvp));
    shader.setVec4("uColor", m_color.x, m_color.y, m_color.z, 0.6f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

}  // namespace adofai
