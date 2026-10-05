add_library(adocao_render STATIC
    Camera.cpp
    Shader.cpp
    TileShape.cpp
    TileMesh.cpp
    Planet.cpp
    PlanetTrail.cpp
)
# 注意：TileGeometryReference.cpp（改造前的 CPU 几何）是**只进测试**的 A/B 基准
# （tests/tile_expansion_test.cpp 用它逐位钉住新实现），渲染路径不依赖它。

target_include_directories(adocao_render PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_render
    PUBLIC adocao_core
    PRIVATE adocao_glad
)
