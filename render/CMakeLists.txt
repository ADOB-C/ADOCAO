add_library(adocao_render STATIC
    Camera.cpp
    Shader.cpp
    TileShape.cpp
    TileGeometryReference.cpp   # 过渡：TileMesh 还没换掉（S3 换完从这里删掉，只留测试里的那份）
    TileMesh.cpp
    Planet.cpp
    PlanetTrail.cpp
)
# 长远：TileGeometryReference.cpp（改造前的 CPU 几何）应该是**只进测试**的 A/B 基准，
# 渲染路径不再依赖它 —— S3 把 TileMesh 改成消费 TileShape 之后就从这里删掉。

target_include_directories(adocao_render PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_render
    PUBLIC adocao_core
    PRIVATE adocao_glad
)
