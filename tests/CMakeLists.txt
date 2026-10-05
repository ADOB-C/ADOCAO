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
