// L2：**GPU 端**逐位对拍 —— 真正跑 `assets/shaders/tile.vert`，把每个 (形状, canonical 槽位)
// 的局部坐标写进 RGBA32F 的 FBO，再 `glReadPixels(GL_FLOAT)` 读回来，与 CPU 的 expand() 逐位比。
//
// 为什么 CPU 端（tests/tile_expansion_test.cpp）不够：那边证明的是"算式结构在我这边一致"，
// 但真正算坐标的是**驱动**的编译器 —— 它可能把 `a*b+c` 融合/不融合、把 `fma(a,b,0)` 化简、
// 或者对 `-0.0` 有别的想法。只有把 GPU 的输出读回来比一遍，才谈得上"改完和原来完全一致"。
//
// 做法与取舍：
//   * GLSL 源码**从文件读**，前面塞 `#define GEOM_PROBE 1` —— 测的就是生产那份源码，不是副本。
//   * 画 `GL_POINTS`：顶点号 = canonical 槽号（0..174，EBO 顺序），实例号 = 形状号；
//     点图元不插值，所以 FS 拿到的 varying 就是 VS 算出来的原值。
//   * 只比 x/y（z 对砖恒为 0、对图标是各自的 z 偏移），并单独断言图标 z 的三个常量。
//   * 没有显示/驱动（CI 的 headless Linux）时打印 SKIP 退 0 —— 不能因为没 GPU 就把 CI 弄红。
//
// 用法：`./build/tests/adocao_geom_probe_test [chart.adofai …]`（默认用仓库里那两张 fixture）

#include "TileShape.hpp"
#include "glad/gl_core.hpp"
#include "GLFW/glfw3.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

using namespace TileShape;

namespace {

int g_fail = 0;

std::string bits(float f) {
    uint32_t u; std::memcpy(&u, &f, 4);
    char b[32]; std::snprintf(b, sizeof b, "0x%08x (%.9g)", u, (double)f);
    return b;
}
// ±0 视为同值：光栅化分不出 -0.0 与 +0.0（下游只有加减乘，没有 sign()/1/x），所以像素上
// 严格等价。区别只可能来自驱动对取负/零符号的内部处理，这里单独计数、不当失败。
bool sameValue(float a, float b) {
    if (std::memcmp(&a, &b, 4) == 0) return true;
    return a == 0.0f && b == 0.0f;
}

// 默认容忍 ≤2 ULP：这台机器（Apple/Metal，clang 钉的融合结构）实测逐位相同，但**别的驱动**
// 可能把 fma 融合成别的东西 —— 那在 8bit 输出上看不出来（见 AGENTS 的标定），没必要让别家 GPU
// 上跑 ctest 直接红。`ADOCAO_TILE_EXACT=1` 恢复逐位要求（本地验收/换驱动时用）。
bool g_requireExact = false;
// 与 tile_expansion 同一口径：默认按**几何尺度**的绝对容差，而不是输出值的 ULP
// （y 有相消的槽位上，中间量差 1 ULP 会放大成几十 ULP，但绝对偏差 ~1e-8 砖）。
constexpr double kTol = 1e-5;

void fail(const char* fmt, ...) {
    if (g_fail < 12) {
        va_list ap; va_start(ap, fmt);
        std::fputs("  ✗ ", stderr); std::vfprintf(stderr, fmt, ap); std::fputc('\n', stderr);
        va_end(ap);
    }
    g_fail++;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::string shaderDir(int argc, char** argv) {
    // 与 LevelScene 的资产查找同序：CWD → 可执行文件目录 → 往上一级
    std::vector<std::string> cands = {"assets/shaders/", "build/assets/shaders/",
                                      "../assets/shaders/", "../../assets/shaders/"};
    (void)argc; (void)argv;
    for (const auto& c : cands)
        if (!readFile(c + "tile.vert").empty()) return c;
    return "assets/shaders/";
}

GLuint compile(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        std::vector<char> log(8192, 0);
        glGetShaderInfoLog(s, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "着色器编译失败：\n%s\n", log.data());
        return 0;
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (const char* e = std::getenv("ADOCAO_TILE_EXACT")) g_requireExact = (std::atoi(e) != 0);
    // 形状集：命令行给的谱面 + 0.01° 抽样（与 L1 同源，保证两边比的是同一批形状）
    std::vector<Shape> shapes;
    std::vector<std::pair<std::pair<float, float>, bool>> keys;
    auto addKey = [&](float sa, float ea, bool mid) { keys.push_back({{sa, ea}, mid}); };
    for (int i = 1; i < argc; i++) {
        LevelData lv;
        if (!lv.loadFromFile(argv[i])) { std::fprintf(stderr, "  ! 读不了谱面: %s\n", argv[i]); continue; }
        int n = (int)lv.tiles.size() - 1;
        for (int t = 0; t < n; t++) {
            float sa, ea; bool mid;
            keyForTile(lv, t, sa, ea, mid);
            addKey(sa, ea, mid);
        }
    }
    for (float base = 0.0f; base < 360.0f; base += 0.37f) {
        addKey(base, std::fmod(base + 90.0f, 360.0f), false);
        addKey(base, std::fmod(base + 0.005f, 360.0f), false);
        addKey(base, base, false);
        addKey(base, std::fmod(base + 180.0f, 360.0f), false);
        addKey(base, std::fmod(base + 180.0f, 360.0f), true);
    }
    const float axis[] = {0.0f, 90.0f, 180.0f, 270.0f, -180.0f};
    for (float a : axis) for (float b : axis) { addKey(a, b, false); addKey(a, b, true); }
    shapes.reserve(keys.size());
    for (auto& kv : keys) shapes.push_back(buildShape(kv.first.first, kv.first.second, kv.second));
    std::printf("== 形状集：%zu 个 ==\n", shapes.size());

    if (!glfwInit()) { std::printf("SKIP：glfwInit 失败（无显示？）—— CI 上不算失败\n"); return 0; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "geom-probe", nullptr, nullptr);
    if (!win) { std::printf("SKIP：无法创建 GL 窗口（无显示/无驱动）\n"); glfwTerminate(); return 0; }
    glfwMakeContextCurrent(win);
    if (!loadGLCore()) {
        std::printf("SKIP：glad 加载失败\n"); glfwTerminate(); return 0;
    }
    std::printf("   GL %s | GLSL %s\n", (const char*)glGetString(GL_VERSION),
                (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));

    // ---- 形状表 → RGBA32F 纹理 ----
    std::vector<float> texels;
    int rows = packShapeTable(shapes.data(), (int)shapes.size(), texels);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kTexW, rows, 0, GL_RGBA, GL_FLOAT, texels.data());

    // ---- 配方表（175 槽）+ 点索引（0..174）----
    const Recipe* rc = recipeTable();
    std::vector<float> recipes;
    recipes.reserve((size_t)kTotalSlots * 4);
    for (int i = 0; i < kTotalSlots; i++) {
        recipes.push_back(rc[i].part);   // part id（0..5 砖，6..8 图标）
        recipes.push_back(rc[i].slot);   // layer*64 + 槽号（图标恒为槽号）
        recipes.push_back(rc[i].k0);
        recipes.push_back(rc[i].k1);
    }

    std::vector<uint16_t> pointIdx(kTotalSlots);
    for (int i = 0; i < kTotalSlots; i++) pointIdx[(size_t)i] = (uint16_t)i;

    GLuint vao = 0, vbo = 0, ebo = 0, instVbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(recipes.size() * sizeof(float)), recipes.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glGenBuffers(1, &ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(pointIdx.size() * sizeof(uint16_t)), pointIdx.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &instVbo); glBindBuffer(GL_ARRAY_BUFFER, instVbo);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(3, 1);

    // ---- FBO：宽 kTotalSlots（+1 边距），高 = 一批形状数 ----
    const int batch = 512;
    int fbW = kTotalSlots + 2, fbH = batch;
    GLuint fbo = 0, colorTex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, fbW, fbH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("✗ RGBA32F FBO 不完整（驱动不支持浮点渲染目标？）\n");
        return 1;
    }

    // ---- 着色器：tile.vert 前面塞 GEOM_PROBE ----
    const std::string dir = shaderDir(argc, argv);
    std::string vs = readFile(dir + "tile.vert");
    if (vs.empty()) { std::printf("✗ 读不到 %stile.vert\n", dir.c_str()); return 1; }
    // `#version` 必须是第一行，所以 define 插在它后面
    size_t nl = vs.find('\n');
    vs = vs.substr(0, nl + 1) + "#define GEOM_PROBE 1\n" + vs.substr(nl + 1);
    const char* fs = R"(#version 410 core
in vec4 vProbe;
out vec4 fragColor;
void main() { fragColor = vProbe; }
)";
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v); glAttachShader(prog, f);
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { std::printf("✗ 着色器链接失败\n"); return 1; }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uShapeTex"), 0);
    glUniform1i(glGetUniformLocation(prog, "uTexW"), kTexW);
    glUniform2f(glGetUniformLocation(prog, "uProbeSize"), (float)fbW, (float)fbH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    std::vector<float> readback((size_t)fbW * fbH * 4);
    std::vector<float> inst((size_t)batch * 5);
    // 注意别叫 near/far：Windows 的 <windows.h> 把这两个当宏（MinGW 直接编不过）
    long long checked = 0, zeroSignOnly = 0, exact = 0, withinTol = 0;
    double maxDev = 0.0;
    for (size_t base = 0; base < shapes.size(); base += batch) {
        int n = (int)std::min((size_t)batch, shapes.size() - base);
        for (int i = 0; i < n; i++) {
            inst[(size_t)i * 5 + 0] = 0.0f;                 // offX
            inst[(size_t)i * 5 + 1] = 0.0f;                 // offY
            inst[(size_t)i * 5 + 2] = 0.0f;                 // offZ（砖 Z 由这里进，probe 里为 0）
            inst[(size_t)i * 5 + 3] = (float)(base + (size_t)i);   // 形状号
            inst[(size_t)i * 5 + 4] = 7.0f;                 // iconBits：三种图标全开（也查图标槽位）
        }
        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * 5 * sizeof(float)), inst.data(), GL_DYNAMIC_DRAW);
        glViewport(0, 0, fbW, fbH);
        glDisable(GL_DEPTH_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElementsInstanced(GL_POINTS, kTotalSlots, GL_UNSIGNED_SHORT, nullptr, n);
        glFinish();
        glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_FLOAT, readback.data());

        for (int i = 0; i < n; i++) {
            Expanded ex;
            expand(shapes[base + (size_t)i], ex);
            for (int s = 0; s < kTotalSlots; s++) {
                if (s >= fbW) break;
                const float* got = &readback[((size_t)i * fbW + (size_t)s) * 4];
                const float* want = &ex.pos[(size_t)s * 3];
                for (int c = 0; c < 2; c++) {
                    checked++;
                    const bool sameBits = (std::memcmp(&got[c], &want[c], 4) == 0);
                    const double dev = std::fabs((double)got[c] - (double)want[c]);
                    if (sameBits) exact++;
                    else if (std::fabs(got[c]) == 0.0 && std::fabs(want[c]) == 0.0) zeroSignOnly++;
                    else if (dev <= kTol) withinTol++;
                    if (dev > maxDev) maxDev = dev;
                    if (dev > kTol || (g_requireExact && !sameValue(got[c], want[c]))) {
                        fail("形状 %zu 槽 %d %s: GPU %s ≠ CPU %s（偏差 %.3g）", base + (size_t)i, s,
                             c ? "y" : "x", bits(got[c]).c_str(), bits(want[c]).c_str(), dev);
                    }
                }
                // 图标 z 的三个常量（砖的 z 恒 0，由实例偏移里的砖 Z 负责）
                if (s >= kTileSlots) {
                    int part = (s - kTileSlots) / kIconSlots;
                    float wantZ = (part == 0) ? kIconZBase : (kIconZBase + kIconZExtra);
                    if (std::memcmp(&got[2], &wantZ, 4) != 0) {
                        fail("形状 %zu 图标槽 %d z: GPU %s ≠ %s", base + (size_t)i, s,
                             bits(got[2]).c_str(), bits(wantZ).c_str());
                    }
                } else if (got[2] != 0.0f) {
                    fail("形状 %zu 砖槽 %d z: GPU %s ≠ 0", base + (size_t)i, s, bits(got[2]).c_str());
                }
            }
        }
        if (g_fail > 12) break;
    }

    std::printf("对拍坐标 %lld 个（x/y × %zu 形状 × %d 槽），失败 %d\n",
                checked, shapes.size(), kTotalSlots, g_fail);
    std::printf("  逐位相同 %lld（其中仅差零符号 %lld），非逐位但在 %.0e 内 %lld，最大偏差 %.3g%s\n",
                exact, zeroSignOnly, kTol, withinTol, maxDev,
                g_requireExact ? "（ADOCAO_TILE_EXACT=1：要求逐位）"
                               : "（默认按几何尺度容差）");
    glfwDestroyWindow(win);
    glfwTerminate();
    if (g_fail) { std::printf("FAILED\n"); return 1; }
    std::printf("OK\n");
    return 0;
}
