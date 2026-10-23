add_library(adocao_render STATIC
    Camera.cpp
    Shader.cpp
    TileShape.cpp
    TileMesh.cpp
    Planet.cpp
    PlanetTrail.cpp
    LevelScene.cpp
)
# 注意：TileGeometryReference.cpp（改造前的 CPU 几何）是**只进测试**的 A/B 基准
# （tests/tile_expansion_test.cpp 用它逐位钉住新实现），渲染路径不依赖它。

target_include_directories(adocao_render PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_render
    PUBLIC adocao_core
    PRIVATE adocao_glad
)

# 注意：`LevelScene` 放在 render/ 而不是 app/，因为 **app/** 不进 ADOFAI.Lib 的镜像**。
# 下游（ADOFAO-E）曾经整份复制过它的 577 行 —— 复制品一定会漂移，所以它属于库。
# 宿主要求的注入点只有一个：`SceneConfig::residentBytes`（RSS 日志；库不依赖 app/ProcMem）。
