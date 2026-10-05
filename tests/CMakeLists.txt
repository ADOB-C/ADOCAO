# 关卡解析对拍测试：快路径必须与 cleanJson + RapidJSON 老路径逐位一致
add_executable(adocao_level_parse_test level_parse_test.cpp)
target_link_libraries(adocao_level_parse_test PRIVATE adocao_core)

add_test(NAME level_parse_parity
         COMMAND adocao_level_parse_test "${CMAKE_CURRENT_SOURCE_DIR}/level_fixtures")

# 砖块几何自测：中旋砖（angleData=999）必须是五边形（尖角朝来路、面积 3w²）；带负向对照。
# TileGeometryReference.cpp（改造前的 CPU 几何，现在的 A/B 基准）只需要 <vector>/<cmath>，
# 直接编进测试，不碰 GL。
add_executable(adocao_tile_geometry_test
               tile_geometry_test.cpp
               ${CMAKE_SOURCE_DIR}/render/TileGeometryReference.cpp)
target_include_directories(adocao_tile_geometry_test PRIVATE ${CMAKE_SOURCE_DIR}/render)

add_test(NAME tile_geometry
         COMMAND adocao_tile_geometry_test ${CMAKE_SOURCE_DIR}/render/TileMesh.cpp)

# L1 几何逐位对拍：新的"canonical part 记录 + VS 展开"必须与参考实现逐位相同
# （参数是从 sa/ea 重新推出来的，FMA 融合顺序差 1 ULP 就会在 zoom 250 上翻像素）。
# 形状集 = tests/charts/*.adofai 的每个砖 + 0.01° 网格抽样；TileShape.cpp 不碰 GL。
add_executable(adocao_tile_expansion_test
               tile_expansion_test.cpp
               ${CMAKE_SOURCE_DIR}/render/TileShape.cpp
               ${CMAKE_SOURCE_DIR}/render/TileGeometryReference.cpp)
target_include_directories(adocao_tile_expansion_test PRIVATE ${CMAKE_SOURCE_DIR}/render)
target_link_libraries(adocao_tile_expansion_test PRIVATE adocao_core)

add_test(NAME tile_expansion
         COMMAND adocao_tile_expansion_test
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/angles360.adofai"
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/shapes_exhaustive.adofai")

# L2 GPU 逐位对拍：真正跑 assets/shaders/tile_v2.vert，把局部坐标读回来与 CPU 的 expand() 比。
# 需要 GL 上下文；没有显示/驱动时测试自己 SKIP 退 0（CI 的 headless Linux 就不会红）。
add_executable(adocao_geom_probe_test
               geom_probe_test.cpp
               ${CMAKE_SOURCE_DIR}/render/TileShape.cpp)
target_include_directories(adocao_geom_probe_test PRIVATE ${CMAKE_SOURCE_DIR}/render)
target_link_libraries(adocao_geom_probe_test PRIVATE adocao_core adocao_glad)
# glad 是手写的最小加载器（只列项目用到的入口），没有 glReadPixels/glFinish —— 探针直接用
# 平台 GL（和 app/CMakeLists.txt 同一个写法）。平时渲染路径不需要这些。
if(WIN32)
    target_link_libraries(adocao_geom_probe_test PRIVATE opengl32)
elseif(APPLE)
    target_link_libraries(adocao_geom_probe_test PRIVATE "-framework OpenGL")
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(adocao_geom_probe_test PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
endif()

add_test(NAME geom_probe
         COMMAND adocao_geom_probe_test
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/angles360.adofai"
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/shapes_exhaustive.adofai")
