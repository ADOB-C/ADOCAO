# 关卡解析对拍测试：快路径必须与 cleanJson + RapidJSON 老路径逐位一致
add_executable(adocao_level_parse_test level_parse_test.cpp)
# 这个用例直接用了 ArchiveStream（半窗流式对拍），所以要连压缩库的头一起
target_link_libraries(adocao_level_parse_test PRIVATE adocao_core adocao_archive liblzma libzstd_static)

add_test(NAME level_parse_parity
         COMMAND adocao_level_parse_test "${CMAKE_CURRENT_SOURCE_DIR}/level_fixtures")

# lzma_mt 内存字段的跨版本兼容（5.4 起 `memlimit` 拆成 memlimit_threading/memlimit_stop）：
# 纯编译期选择 + 断言，不依赖压缩库，也不链任何 target。
add_executable(adocao_lzma_mt_compat_test lzma_mt_compat_test.cpp)
target_include_directories(adocao_lzma_mt_compat_test PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME lzma_mt_compat COMMAND adocao_lzma_mt_compat_test)
add_executable(adocao_timeline_ab_dump_test timeline_ab_dump.cpp)
target_include_directories(adocao_timeline_ab_dump_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_timeline_ab_dump_test PRIVATE adocao_core)
add_test(NAME timeline_ab_dump COMMAND adocao_timeline_ab_dump_test ${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai)
add_executable(adocao_tl_trail_dump tl_trail_dump.cpp)
target_include_directories(adocao_tl_trail_dump PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_tl_trail_dump PRIVATE adocao_core)
add_executable(adocao_tl_full_dump tl_full_dump.cpp)
target_include_directories(adocao_tl_full_dump PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_tl_full_dump PRIVATE adocao_core)
add_executable(adocao_tl_dump tl_dump.cpp)
target_include_directories(adocao_tl_dump PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_tl_dump PRIVATE adocao_core)
add_executable(adocao_limits_test limits_test.cpp)
target_include_directories(adocao_limits_test PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME limits COMMAND adocao_limits_test)

# 快路径必须吃下非整数 action 字段（parseNumber 曾把"token 正好填满值跨度"误判为截断，
# 于是所有含小数 action 字段的谱整份退回旧路径）。进程外跑，带负向对照。
add_test(NAME fast_path_noninteger COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-fast-path.sh)

# `.adocao` 列编码器自测：逐位往返 + 编码选择（字典/常量/差分/位打包）+ 坏数据必须干净失败
# （截断、count 说谎、未知 codec、越界下标）。只链 archive（列编码器不用压缩库）。
add_executable(adocao_columns_test adocao_columns_test.cpp)
target_link_libraries(adocao_columns_test PRIVATE adocao_archive)
add_test(NAME adocao_columns COMMAND adocao_columns_test)

# `.adocao` v3 判据统计（§8.4）：哨兵掩码 + 差分基数（raw vs wrapped）。
# 关键断言是**负向对照**：不掩码时中旋哨兵会把"环绕率"抬成假象；全精度正弦的窗口永远不重复
# （即 `Motif` 要的是**逐位重复**）。只链 archive（列编码器不用压缩库）。
add_executable(adocao_diagnostics_test adocao_diagnostics_test.cpp)
target_link_libraries(adocao_diagnostics_test PRIVATE adocao_archive)
add_test(NAME adocao_diagnostics COMMAND adocao_diagnostics_test)

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

# PositionTrack 的 justThisTile 消费侧语义（点加 vs 后缀加 + 负向对照）
add_executable(adocao_position_offset_test position_offset_test.cpp)
target_include_directories(adocao_position_offset_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_position_offset_test PRIVATE adocao_core)
add_test(NAME position_offset COMMAND adocao_position_offset_test)

# audio-as-chart（raw-PCM 直通）：整谱就是波形 —— 这条链决定"能不能听见和弦"（do-mi-sol）。
# 三层断言：buildRawPcm 认谱 + **负向对照**（普通打拍音谱必须被拒）+ 重采样后三个分音
# 同时远高于对照频率。要链 audio（HitsoundManager 在这里）。
add_executable(adocao_raw_pcm_test raw_pcm_test.cpp)
target_include_directories(adocao_raw_pcm_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_raw_pcm_test PRIVATE adocao_core adocao_audio)
add_test(NAME raw_pcm
         COMMAND adocao_raw_pcm_test
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_wav_rate.adofai"
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_dict.adofai"
                 "${CMAKE_BINARY_DIR}/diag/wav_rate.wav")

# "拿 Kick 打出一个和弦"：击打时刻放在 Do/Mi/Sol 三个频率网格的**并集**上
# （角度序列 = 相位累积 → 砖路画成花环，耳朵听到和弦）。三层断言：时刻对齐并集网格、
# 混音后三条谱线同时高于本底、能导出 WAV。要链 audio + 读 assets/hitsounds。
add_executable(adocao_kick_chord_test kick_chord_test.cpp)
target_include_directories(adocao_kick_chord_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_kick_chord_test PRIVATE adocao_core adocao_audio)
# "自然弯曲"档的护栏：纯 angleData 的赫兹谱（零事件 + 载波 ≥ 最大间隙 + 击打落在音符网格）
add_executable(adocao_hz_grid_test hz_grid_test.cpp)
target_include_directories(adocao_hz_grid_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_hz_grid_test PRIVATE adocao_core)
add_test(NAME hz_grid
         COMMAND adocao_hz_grid_test
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_arpeggio.adofai"
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_duet.adofai")

add_test(NAME kick_chord
         COMMAND adocao_kick_chord_test
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_chord.adofai"
                 "${CMAKE_SOURCE_DIR}/assets/hitsounds"
                 "${CMAKE_BINARY_DIR}/diag/chord_kick.wav")
