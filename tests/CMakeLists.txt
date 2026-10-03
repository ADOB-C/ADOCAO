# 关卡解析对拍测试：快路径必须与 cleanJson + RapidJSON 老路径逐位一致
add_executable(adocao_level_parse_test level_parse_test.cpp)
target_link_libraries(adocao_level_parse_test PRIVATE adocao_core)

add_test(NAME level_parse_parity
         COMMAND adocao_level_parse_test "${CMAKE_CURRENT_SOURCE_DIR}/level_fixtures")
