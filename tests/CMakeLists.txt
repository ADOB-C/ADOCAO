# 关卡解析对拍测试：快路径必须与 cleanJson + RapidJSON 老路径逐位一致
add_executable(adocao_level_parse_test level_parse_test.cpp)
target_link_libraries(adocao_level_parse_test PRIVATE adocao_core)

add_test(NAME level_parse_parity
         COMMAND adocao_level_parse_test "${CMAKE_CURRENT_SOURCE_DIR}/level_fixtures")

# 砖块几何自测：中旋砖（angleData=999）必须是五边形（尖角朝来路、面积 3w²），
# 且描边顶点必须整段排在填充顶点前面 —— TileMesh.cpp 靠扫描 types 前缀算 strokeVertCount，
# 层次一旦交错，画错是静默的。带负向对照（旧几何必须过不了）。
# TileGeometry.cpp 只需要 <vector>/<cmath>，直接编进测试，不碰 GL。
add_executable(adocao_tile_geometry_test
               tile_geometry_test.cpp
               ${CMAKE_SOURCE_DIR}/render/TileGeometry.cpp)
target_include_directories(adocao_tile_geometry_test PRIVATE ${CMAKE_SOURCE_DIR}/render)

add_test(NAME tile_geometry
         COMMAND adocao_tile_geometry_test ${CMAKE_SOURCE_DIR}/render/TileMesh.cpp)
