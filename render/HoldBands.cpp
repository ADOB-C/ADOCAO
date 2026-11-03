#include "render/HoldBands.hpp"

#include "render/PlanetTrail.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/TileXform.hpp"
#include "render/Camera.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "render/Shader.hpp"

#include "render/HoldBands.hpp"

#include <algorithm>
#include "core/util/Logger.hpp"
#include <cmath>

namespace adofai {

void HoldBands::destroy() {
    for (Band& b : m_bands) {
        if (b.ebo) glDeleteBuffers(1, &b.ebo);
        if (b.vbo) glDeleteBuffers(1, &b.vbo);
        if (b.vao) glDeleteVertexArrays(1, &b.vao);
    }
    m_bands.clear();
    // 贴图保留（重建时不重复生成）；进程退出由驱动回收。
}

// 程序生成"彩虹长按带"贴图（照官方 holdTex.png 的**实测**特征，不引外部资产）：
//   官方图 50×200，沿路径（y）约 **2 轮**彩虹、HSV 饱和度 ≈0.55，
//   横跨带宽（x）颜色不变、alpha 由边缘 0 升到中心 255，并有一条**白核**（中心列全白）。
static GLuint makeHoldTexture() {
    const int W = 64, H = 256;
    std::vector<unsigned char> px((size_t)W * H * 4, 0);
    constexpr float kCycles = 2.0f;      // 官方实测：一条带子里约两轮
    constexpr float kSat = 0.55f;        // 最暗通道 ≈116/255
    for (int y = 0; y < H; ++y) {
        const float t = (float)y / (float)(H - 1);
        const float h = std::fmod(t * kCycles, 1.0f) * 6.0f;
        const int i = (int)std::floor(h) % 6;
        const float f = h - std::floor(h);
        const float p = 1.0f - kSat, q = 1.0f - kSat * f, u = 1.0f - kSat * (1.0f - f);
        float r = 0, g = 0, b = 0;
        switch (i) {
            case 0: r = 1; g = u; b = p; break;
            case 1: r = q; g = 1; b = p; break;
            case 2: r = p; g = 1; b = u; break;
            case 3: r = p; g = q; b = 1; break;
            case 4: r = u; g = p; b = 1; break;
            default: r = 1; g = p; b = q; break;
        }
        for (int x = 0; x < W; ++x) {
            const float tx = (float)x / (float)(W - 1);
            const float d = std::fabs(tx - 0.5f) * 2.0f;    // 0 中心 → 1 边缘
            const float edge = 1.0f - d;
            const float a = edge * edge;                     // 官方：边缘 0、中心 255，指数型衰减
            const float core = std::max(0.0f, 1.0f - d / 0.25f);   // 中央白核
            const float rr = r + (1.0f - r) * core;
            const float gg = g + (1.0f - g) * core;
            const float bb = b + (1.0f - b) * core;
            const size_t o = ((size_t)y * W + x) * 4;
            px[o + 0] = (unsigned char)(rr * 255);
            px[o + 1] = (unsigned char)(gg * 255);
            px[o + 2] = (unsigned char)(bb * 255);
            px[o + 3] = (unsigned char)(a * 255);
        }
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void HoldBands::build(const LevelData& lv, const Timeline& tl) {
    destroy();
    if (m_tex == 0) m_tex = makeHoldTexture();
    if (lv.m_tileHoldLength.empty()) return;

    const int64_t n = (int64_t)lv.tiles.size();
    for (const auto& kv : lv.m_tileHoldLength) {
        const int64_t i = kv.first;
        if (i < 0 || i >= n - 1) continue;                 // 需要下一砖
        if (kv.second < 0) continue;

        // ── 几何参数（逐字照抄参考的 UpdateMesh）──────────────────────────
        const auto pa = lv.tilePositionAt(i);
        const auto na = lv.tilePositionAt(i + 1);
        const glm::dvec2 p{pa[0], pa[1]};
        const glm::dvec2 nx{na[0], na[1]};
        const double exitAngle = (double)tl.startAngleAt(i) + (double)tl.totalAngleAt(i);
        const double rStart = (double)lv.radiusScaleAt(i);
        const double rTail  = (double)lv.radiusScaleAt(i + 1);
        const double endCx = nx.x - std::cos(exitAngle) * rTail;
        const double endCy = nx.y - std::sin(exitAngle) * rTail;
        const double startAngle = (double)tl.startAngleAt(i);
        const double totalAngle = (double)tl.totalAngleAt(i);
        const double width = std::max(0.15, 0.275 * (rStart + rTail) * 0.5);

        const double absTotal = std::fabs(totalAngle);
        const int segs = (int)std::max(24.0, std::min(512.0, std::ceil(absTotal / 0.08)));
        const double half = width * 0.5;

        auto sample = [&](double t, double& x, double& y) {
            const double a = startAngle + totalAngle * t;
            const double cx = p.x + (endCx - p.x) * t;
            const double cy = p.y + (endCy - p.y) * t;
            const double r = rStart + (rTail - rStart) * t;
            x = cx + std::cos(a) * r;
            y = cy + std::sin(a) * r;
        };

        std::vector<float> verts;      // 5 float/顶点：pos.xyz + uv.xy
        std::vector<unsigned> idx;
        verts.reserve((size_t)(segs + 1) * 10);
        idx.reserve((size_t)segs * 6);
        for (int s = 0; s <= segs; ++s) {
            const double t = (double)s / (double)segs;
            double x0, y0, xa, ya, xb, yb;
            sample(t, x0, y0);
            sample(std::max(0.0, t - 1.0 / 512.0), xa, ya);
            sample(std::min(1.0, t + 1.0 / 512.0), xb, yb);
            double tx = xb - xa, ty = yb - ya;
            const double len = std::sqrt(tx * tx + ty * ty);
            if (len > 0.0) { tx /= len; ty /= len; }
            const double nxx = -ty, nyy = tx;
            // **顶点存"相对起点圆心"的局部坐标**：绘制时按 (center + MoveTrack 位移 − 相机) 平移。
            // 世界坐标直存会与 draw 里的相机相对化重复叠加一次（第一次就是因此整条带飞出画面）。
            verts.insert(verts.end(), {(float)(x0 - p.x - nxx * half), (float)(y0 - p.y - nyy * half),
                                       0.5f, 0.0f, (float)t});
            verts.insert(verts.end(), {(float)(x0 - p.x + nxx * half), (float)(y0 - p.y + nyy * half),
                                       0.5f, 1.0f, (float)t});
            if (s > 0) {
                const unsigned b = (unsigned)((s - 1) * 2);
                idx.insert(idx.end(), {b, b + 1, b + 2, b + 1, b + 3, b + 2});
            }
        }

        Band band;
        band.floor = i;
        band.center = glm::dvec2(p.x, p.y);
        // **必须用 startTimeAt()**：大谱会 `dropStartTimesArray()` 省 54 MB，密集数组是空的。
        band.startTime = tl.startTimeAt(i);
        band.endTime = tl.startTimeAt(i + 1);
        band.indexCount = (int)idx.size();
        for (size_t k = 0; k < 15 && k < verts.size(); ++k) band.probe[k] = verts[k];

        glGenVertexArrays(1, &band.vao);
        glBindVertexArray(band.vao);
        glGenBuffers(1, &band.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, band.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glGenBuffers(1, &band.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, band.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(unsigned)), idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);

        {
            float x0=1e9f,x1=-1e9f,y0=1e9f,y1=-1e9f;
            for (size_t k=0;k+4<verts.size();k+=5){x0=std::min(x0,verts[k]);x1=std::max(x1,verts[k]);
                                                  y0=std::min(y0,verts[k+1]);y1=std::max(y1,verts[k+1]);}
            LOG_I("hold-bbox: 局部 x=[%.2f,%.2f] y=[%.2f,%.2f] 段=%d", x0,x1,y0,y1,segs);
        }
        m_bands.push_back(band);
    }
    LOG_I("HoldBands: %zu 条长按带（事件 %zu 条）", m_bands.size(), lv.m_tileHoldLength.size());
}

void HoldBands::draw(Shader& shader, const Camera& camera, double camX, double camY, double timeSec,
                     const LevelData& lv, const TileAnimation* anim) {
    if (m_bands.empty()) return;
    shader.use();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    // **必须关面剔除**：带子是薄片，背朝相机的段会被 GL_BACK 剔掉。
    // 之前"顶点合法、程序链好、无 GL 错误、NDC 在屏幕正中，却一个像素都没有"就是它
    // （连硬编码 `FragColor = vec4(1,0,1,1)` 都不出像素 ——fragment 根本没跑）。
    // 参考实现用 `side: DoubleSide`，等价於这里关剔除。
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    {
        const GLuint prog = (GLuint)shader.id();
        const GLint loc = glGetUniformLocation(prog, "uMap");
        if (loc >= 0) glUniform1i(loc, 0);
    }
    for (const Band& b : m_bands) {
        const double dur = b.endTime - b.startTime;
        const double prog = dur > 0.0 ? (timeSec - b.startTime) / dur : 0.0;
        // **不要"没到就不画"**：参考 `updateHoldRenderers` 里带子**常驻在场景**，
        // 完成度 clamp 到 0..1，可见性只跟砖的透明度走。所以暂停在长按之前时，
        // 带子应以"未填充"的样子显示；以前这里 `prog < -0.02 → continue`，
        // 于是**暂停时整条带子消失**（用户报的就是这个）。
        // 起点砖的 MoveTrack/AnimateTrack 位移：整条带跟着平移（官方挂在起点砖下）
        const TileXform xf = tileXformAt(lv, anim, b.floor, timeSec);
        const glm::dvec2 off = b.center + glm::dvec2(xf.moveX, xf.moveY) - glm::dvec2(camX, camY);
        auto model = glm::translate(glm::mat4(1.0f), glm::vec3((float)off.x, (float)off.y, 0.0f));

        const glm::mat4 mvp = camera.viewProj() * model;
        shader.setMat4("uMVP", glm::value_ptr(mvp));
        shader.setFloat("uCompletion", (float)std::clamp(prog, 0.0, 1.0));
        shader.setVec3("uColor", 1.0f, 1.0f, 1.0f);
        // **不用 `tileXformAt().opacity`**：那条路对"已经消失的砖"返回 0，而长按带只在
        // 自己那块砖的这段时间里画（prog ∈ [0,1]），本来就该跟着砖亮；实测用它会让整条带全透明。
        // 透明度跟**起点砖的真实可见性**（AnimateTrack 的 appear/disappear + MoveTrack 的 alpha），
        // 与参考 `setOpacity(tileOpacity)` / `setVisible(tileOpacity > 0.001)` 同构。
        shader.setFloat("uOpacity", (float)std::clamp(xf.opacity, 0.0, 1.0));
        if (xf.opacity <= 0.001) continue;
        glBindVertexArray(b.vao);
        glDrawElements(GL_TRIANGLES, b.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

}  // namespace adofai
