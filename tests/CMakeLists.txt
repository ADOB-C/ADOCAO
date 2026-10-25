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
# 砖色模型：trackStyle 的 fill/stroke 派生 + 颜色类型 + pulse（金标来自 Re_ADOJAS）
add_executable(adocao_tile_color_test tile_color_test.cpp)
target_link_libraries(adocao_tile_color_test PRIVATE adocao_core)
add_test(NAME tile_color COMMAND adocao_tile_color_test)

add_test(NAME fast_path_noninteger COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-fast-path.sh)

# `.adocao` 列编码器自测：逐位往返 + 编码选择（字典/常量/差分/位打包）+ 坏数据必须干净失败
# （截断、count 说谎、未知 codec、越界下标）。只链 archive（列编码器不用压缩库）。
add_executable(adocao_columns_test adocao_columns_test.cpp)
target_link_libraries(adocao_columns_test PRIVATE adocao_archive)
add_test(NAME adocao_columns COMMAND adocao_columns_test)

# `.adocao` v3 分块列容器（§8.3 ⑥）：逐位往返（含 4096 边界附近与 -0.0/NaN/极值）+
# **分段型收益**（§8.3 的 1.66~3.29× 那一档，数字打印留痕）+ 两个负向对照
# （同质/字典主导/白噪声列上分块不许赢 → 最坏零收益）+ 按块随机访问（③ 的地基）+
# 坏数据必须干净失败（截断、帧表越界、kind 不符、blockElems=0、未知 layout）。
add_executable(adocao_blocked_test adocao_blocked_test.cpp)
target_link_libraries(adocao_blocked_test PRIVATE adocao_archive)
add_test(NAME adocao_blocked COMMAND adocao_blocked_test)

# `.adocao` v3 `Derived` 段（§8.8 ⑧）：每 4096 层的续算快照。唯一验收手段是 §8.10⑥ 的
# "从快照重算 position，与全谱一次算 **逐位** memcmp"（含 PositionTrack 的点加/继承、
# 中旋 999、以及 **1 ULP 的反面对照**：动一位必须变红）。链 archive。
add_executable(adocao_checkpoints_test adocao_checkpoints_test.cpp)
target_link_libraries(adocao_checkpoints_test PRIVATE adocao_archive)
add_test(NAME adocao_checkpoints COMMAND adocao_checkpoints_test)

# ③ 每砖结构窗口化：从 checkpoint + 一块数据重算 Tile/tileBPMs/tileStartTimes，必须与全量加载
# **逐位** memcmp 相同（含块边界事件、中旋跨窗、SetSpeed 的 float 累乘）；外加容器里的按层索引
# （ActionIndex）与全量扫描逐块对照、以及"动一位必须变红"的四条反面对照。
add_executable(adocao_window_test adocao_window_test.cpp)
target_link_libraries(adocao_window_test PRIVATE adocao_archive)
add_test(NAME adocao_window COMMAND adocao_window_test)

# `.adocao` v3 判据统计（§8.4）：哨兵掩码 + 差分基数（raw vs wrapped）。
# 关键断言是**负向对照**：不掩码时中旋哨兵会把"环绕率"抬成假象；全精度正弦的窗口永远不重复
# （即 `Motif` 要的是**逐位重复**）。只链 archive（列编码器不用压缩库）。
add_executable(adocao_diagnostics_test adocao_diagnostics_test.cpp)
target_link_libraries(adocao_diagnostics_test PRIVATE adocao_archive)
add_test(NAME adocao_diagnostics COMMAND adocao_diagnostics_test)

# ③ 切片 4：位图区间置值（朴素逐位实现当参照）
add_executable(adocao_bit_span_test bit_span_test.cpp)
target_link_libraries(adocao_bit_span_test PRIVATE adocao_core)
add_test(NAME bit_span COMMAND adocao_bit_span_test)

# ③ 切片 4 另一半：angleData 的紧凑存储（释放前后逐位对拍 + 反面对照）
add_executable(adocao_angle_store_test angle_store_test.cpp)
target_link_libraries(adocao_angle_store_test PRIVATE adocao_core adocao_archive)
add_test(NAME angle_store
         COMMAND adocao_angle_store_test ${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai
                 "${CMAKE_SOURCE_DIR}/tests/charts/diag_glide.adofai")

# ③ 切片 4：按块物化的每砖 provider（丢掉密集 tiles 后逐层对拍 + 反面对照）
add_executable(adocao_floor_window_cache_test floor_window_cache_test.cpp)
target_link_libraries(adocao_floor_window_cache_test PRIVATE adocao_core adocao_archive)
add_test(NAME floor_window_cache
         COMMAND adocao_floor_window_cache_test ${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai
                 "${CMAKE_SOURCE_DIR}/tests/charts/diag_glide.adofai")

# ③ 切片 1：Timeline 锚点（每 4096 层）与全量数组逐位对拍
add_executable(adocao_timeline_anchor_test timeline_anchor_test.cpp)
target_link_libraries(adocao_timeline_anchor_test PRIVATE adocao_core adocao_archive)
add_test(NAME timeline_anchor
         COMMAND adocao_timeline_anchor_test ${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai
                 "${CMAKE_SOURCE_DIR}/tests/charts/diag_glide.adofai")

# ④ 粗块 AABB 索引：与全扫逐元素对拍（含反面对照）
add_executable(adocao_cull_index_test cull_index_test.cpp)
target_include_directories(adocao_cull_index_test PRIVATE ${CMAKE_SOURCE_DIR}/render)
target_link_libraries(adocao_cull_index_test PRIVATE adocao_core adocao_archive)
add_test(NAME cull_index
         COMMAND adocao_cull_index_test ${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai)

# 样式段（trackStyleRunEnd）的区间不变量：run 区间内 trackStyleAt 必须逐字符相同。
# 覆盖 ColorTrack/justThisTile/inactive 与 RecolorTrack（负向范围、angleOffset 90、gap）。
add_executable(adocao_style_run_test style_run_test.cpp)
target_link_libraries(adocao_style_run_test PRIVATE adocao_core adocao_archive)
add_test(NAME style_run
         COMMAND adocao_style_run_test
                 ${CMAKE_SOURCE_DIR}/tests/level_fixtures/f39_color_events.json
                 ${CMAKE_SOURCE_DIR}/tests/level_fixtures/f40_recolor_track.json)

# 剔除策略对比探针（④）：同一份真实数据、同一套 AABB 判定下比三种候选集策略
# （全扫 O(n) / 层号±4096 窗口 / 4096 层块 AABB 查询）的候选数、漏砖数、耗时、索引体积。
add_executable(adocao_cull_cost_probe cull_cost_probe.cpp)
target_link_libraries(adocao_cull_cost_probe PRIVATE adocao_core adocao_archive)
add_test(NAME cull_cost COMMAND adocao_cull_cost_probe ${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai --zooms 25,100 --repeat 3)

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
# 字体链回归：至少一支候选 CJK 字体必须能被 ImGui 真正加载并覆盖 あ/中/가
# （macOS 的 PingFang.ttc 是 fopen 成功但解析失败 —— 旧循环在它身上 break，CJK 全变 ????）
add_executable(adocao_font_cjk_test font_cjk_test.cpp)
target_include_directories(adocao_font_cjk_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_font_cjk_test PRIVATE adocao_imgui)
add_test(NAME font_cjk COMMAND adocao_font_cjk_test)

# "自然弯曲"档的护栏：纯 angleData 的赫兹谱（零事件 + 载波 ≥ 最大间隙 + 击打落在音符网格）
add_executable(adocao_hz_grid_test hz_grid_test.cpp)
target_include_directories(adocao_hz_grid_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_hz_grid_test PRIVATE adocao_core adocao_audio)
add_test(NAME hz_grid
         COMMAND adocao_hz_grid_test
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_arpeggio.adofai"
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_duet.adofai")

add_test(NAME kick_chord
         COMMAND adocao_kick_chord_test
                 "${CMAKE_CURRENT_SOURCE_DIR}/charts/diag_chord.adofai"
                 "${CMAKE_SOURCE_DIR}/assets/hitsounds"
                 "${CMAKE_BINARY_DIR}/diag/chord_kick.wav")

# `.ilytx`（Re_ADOJAS 自研容器：xz(tar(ilybin))）：与参照谱逐位等价 + 紧凑模式 f32 误差 +
# 三个坏样本必须干净失败。样本由 `tests/gen_ilytx_fixtures.mjs` 用**他们的编码器**生成。
add_executable(adocao_ilytx_test ilytx_test.cpp)
target_link_libraries(adocao_ilytx_test PRIVATE adocao_archive)
add_test(NAME ilytx COMMAND adocao_ilytx_test "${CMAKE_CURRENT_SOURCE_DIR}/level_fixtures")

# 关卡对象的**所有权 / 幂等**：LevelData 不可拷贝（缓存所有权）、可安全移动、
# `loadFromBuffer` 两次 == 一次（actions 不许被追加）、settings 的"沿用默认值"例外、
# 以及 `releaseAngleData()` 之后仍能建窗口缓存（过去那里直接读 angleData[i] = UB）。
add_executable(adocao_level_reload_test level_reload_test.cpp)
target_link_libraries(adocao_level_reload_test PRIVATE adocao_core)
add_test(NAME level_reload COMMAND adocao_level_reload_test "${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai")

# 缓动模块（core/util/Easing）：32 个名字 = 谱面 `ease` 字符串全集。金标由 Re_ADOJAS 的
# `Easing.ts` 用 node 直接算出来（tests/easing_gold.inc），容差 1e-12（跨 JS/libm 的 ulp 差）。
add_executable(adocao_easing_test easing_test.cpp)
target_include_directories(adocao_easing_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_easing_test PRIVATE adocao_core)
add_test(NAME easing COMMAND adocao_easing_test)

# 相机时间线（core/camera/CameraTimeline）：MoveCamera 时间 + RepeatEvents 展开 + 排序键。
# 金标手算（Beat/Floor 两种模式、executeOnCurrentFloor、entryBeat 的 -1 哨兵、多 tag、id tie-break）。
add_executable(adocao_camera_timeline_test camera_timeline_test.cpp)
target_include_directories(adocao_camera_timeline_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_camera_timeline_test PRIVATE adocao_core)
add_test(NAME camera_timeline COMMAND adocao_camera_timeline_test)

# 两层摄像机（core/camera/CameraDirector）：**跨语言金标** —— 同一批事件分别喂给
# Re_ADOJAS 的 CameraController.ts（node 直跑）与我们的移植版，逐帧比较。生成器：
# tests/gen_camera_gold.mjs（金标 tests/camera_gold.inc；wrapper 见 gen_camera_gold.sh）。
add_executable(adocao_camera_test camera_test.cpp)
target_include_directories(adocao_camera_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_camera_test PRIVATE adocao_core)
add_test(NAME camera COMMAND adocao_camera_test)

# 两套 zoom 口径的换算（render/Camera）：本类 = 放大倍数（100 → 12 砖），谱面/ADOFAI =
# 越大看得越远（100 → 5.47 砖）。方向搞反会让 settings.zoom=200 变成"放大 2 倍"。
add_executable(adocao_camera_zoom_test camera_zoom_test.cpp)
target_include_directories(adocao_camera_zoom_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_camera_zoom_test PRIVATE adocao_render)
add_test(NAME camera_zoom COMMAND adocao_camera_zoom_test)

add_executable(adocao_keyframes_test keyframes_test.cpp)
target_link_libraries(adocao_keyframes_test PRIVATE adocao_core)
add_test(NAME keyframes COMMAND adocao_keyframes_test)

add_executable(adocao_tile_animation_test tile_animation_test.cpp)
target_link_libraries(adocao_tile_animation_test PRIVATE adocao_core)
add_test(NAME tile_animation COMMAND adocao_tile_animation_test)

add_executable(adocao_time_origin_test time_origin_test.cpp)
target_link_libraries(adocao_time_origin_test PRIVATE adocao_core)
add_test(NAME time_origin COMMAND adocao_time_origin_test)

add_executable(adocao_schema_test adocao_schema_test.cpp)
target_link_libraries(adocao_schema_test PRIVATE adocao_archive)
add_test(NAME adocao_schema COMMAND adocao_schema_test)

add_executable(adocao_event_columns_test adocao_event_columns_test.cpp)
target_link_libraries(adocao_event_columns_test PRIVATE adocao_archive)
add_test(NAME adocao_event_columns COMMAND adocao_event_columns_test)

add_executable(adocao_v7_container_test adocao_v7_container_test.cpp)
target_link_libraries(adocao_v7_container_test PRIVATE adocao_archive)
add_test(NAME adocao_v7_container COMMAND adocao_v7_container_test)

add_executable(adocao_direct_fields_test adocao_direct_fields_test.cpp)
target_link_libraries(adocao_direct_fields_test PRIVATE adocao_core)
add_test(NAME adocao_direct_fields COMMAND adocao_direct_fields_test)

add_executable(adocao_v7_corpus_test adocao_v7_corpus_test.cpp)
target_link_libraries(adocao_v7_corpus_test PRIVATE adocao_archive)
add_test(NAME adocao_v7_corpus COMMAND adocao_v7_corpus_test ${CMAKE_SOURCE_DIR}/tests/charts)

# 容器 → 明文 → 重新打包 的逐字节验收（回写器只存在于 tests/，ADOCAO 不提供导出）
add_executable(adocao_v7_roundtrip_json_test adocao_v7_roundtrip_json_test.cpp AdocaoJsonWriter.cpp)
target_link_libraries(adocao_v7_roundtrip_json_test PRIVATE adocao_archive)
add_test(NAME adocao_v7_roundtrip_json
         COMMAND adocao_v7_roundtrip_json_test
                 ${CMAKE_SOURCE_DIR}/tests/charts/angles360.adofai
                 ${CMAKE_SOURCE_DIR}/tests/charts/camera_basic.adofai
                 ${CMAKE_SOURCE_DIR}/tests/charts/diag_chord.adofai
                 ${CMAKE_SOURCE_DIR}/tests/charts/neon_alpha.adofai)

add_executable(tiff_writer_test tiff_writer_test.cpp)
target_link_libraries(tiff_writer_test PRIVATE adocao_core)
# 系统没有 zlib 时 core 会 FetchContent 一份（zlibstatic），测试也要能解压
target_include_directories(tiff_writer_test PRIVATE ${CMAKE_SOURCE_DIR}/build/_deps/zlib-src)
add_test(NAME tiff_writer COMMAND tiff_writer_test)

add_executable(anim_timing_test anim_timing_test.cpp)
target_link_libraries(anim_timing_test PRIVATE adocao_core)
add_test(NAME anim_timing COMMAND anim_timing_test)

add_executable(sticky_track_test sticky_track_test.cpp)
target_link_libraries(sticky_track_test PRIVATE adocao_core)
add_test(NAME sticky_track COMMAND sticky_track_test)

add_executable(multiplanet_test multiplanet_test.cpp)
target_link_libraries(multiplanet_test PRIVATE adocao_core)
add_test(NAME multiplanet COMMAND multiplanet_test)

# 测试侧工具（不进 ctest、不随产品发布）：重复 N 遍写成 .adocao。
# 按 AGENTS.md「本项目不该具备产出谱面的能力」，这类生成器只放 tests/。
add_executable(gen_repeat_adocao tools/gen_repeat_adocao.cpp)
target_link_libraries(gen_repeat_adocao PRIVATE adocao_core adocao_archive)
