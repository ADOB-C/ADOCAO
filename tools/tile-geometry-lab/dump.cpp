// 把 render/TileGeometry.cpp 的真实输出 dump 成 JSON —— 验证用的就是**编译进游戏的同一份代码**。
// 编译（在仓库根执行；把 <repo> 换成本地仓库路径）：
//   clang++ -std=c++20 -O1 -I<repo>/render <repo>/tools/tile-geometry-lab/dump.cpp \
//       <repo>/render/TileGeometry.cpp -o /tmp/dump
#include "TileGeometry.hpp"
#include <cstdio>
#include <string>
#include <vector>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

static void dump(const std::string& label, const std::string& note, Scratch& sc) {
    std::printf("{\"label\":\"%s\",\"note\":\"%s\",\"verts\":[", label.c_str(), note.c_str());
    for (size_t i = 0; i < sc.verts.size(); i++) std::printf("%s%.6f", i ? "," : "", sc.verts[i]);
    std::printf("],\"types\":[");
    for (size_t i = 0; i < sc.types.size(); i++) std::printf("%s%.0f", i ? "," : "", sc.types[i]);
    std::printf("],\"indices\":[");
    for (size_t i = 0; i < sc.indices.size(); i++) std::printf("%s%u", i ? "," : "", sc.indices[i]);
    std::printf("]}");
}

int main() {
    Scratch sc;
    std::vector<std::string> out;
    std::printf("[");

    bool first = true;
    auto caseOf = [&](const std::string& label, const std::string& note, auto fn) {
        if (!first) std::printf(",");
        first = false;
        sc.clear();
        fn();
        dump(label, note, sc);
    };

    // 旧行为（对照）：中旋走 createTileMesh(a,a) → ang==0 分支 = 大圆 + 方块
    caseOf("旧：createTileMesh(0,0)", "中旋旧路径（圆 r=0.30 + 方块）", [&] { createTileMesh(0, 0, sc); });
    // 新行为：五边形，a1 = 入砖方向
    caseOf("新：createMidSpinMesh(0°)", "入砖方向朝 +x", [&] { createMidSpinMesh(0, sc); });
    caseOf("新：createMidSpinMesh(90°)", "入砖方向朝 +y", [&] { createMidSpinMesh(90, sc); });
    caseOf("新：createMidSpinMesh(-180°)", "入砖方向朝 -x（最上面 i=0 的情形）", [&] { createMidSpinMesh(-180, sc); });
    caseOf("新：createMidSpinMesh(30°)", "斜向", [&] { createMidSpinMesh(30, sc); });
    // 对照：普通 60° 弯砖（未改动的路径）
    caseOf("普通 createTileMesh(0,60)", "参照：普通弯砖", [&] { createTileMesh(0, 60, sc); });

    std::printf("]\n");
    return 0;
}
