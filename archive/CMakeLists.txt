# archive 模块：谱面容器（.adofai.xz / .adofai.zst）的解压实现。
# 依赖倒置：core 只定义 ByteSource 接口，本模块实现并通过 adofai::archive::install() 注册。
# lzma/zstd 只挂在这里 —— 只想解析明文谱的消费者链 core 即可，不用背压缩库。
add_library(adocao_archive STATIC
    LevelArchive.cpp
    # `.adocao` 的列编码器（字典/常量/差分/位打包/原始）。它**不依赖** lzma/zstd：
    # 压缩只是列流之后的可选一步，设计核心是"每列按取值基数选编码"。
    AdocaoColumns.cpp
    # v7 的 kind 与自描述 schema（类型/字段自动发现；新增事件类型不改格式）
    AdocaoKinds.cpp
    AdocaoSchema.cpp
    AdocaoEventColumns.cpp
    # v3 的分块列容器（4096 一块、块内自选编码、帧级 zstd 与 seek 粒度解耦，§8.3）。
    # 这个文件用 zstd（帧），所以它和 Columns 不同 —— 帧是 seek 的单位，必须能单独解压。
    AdocaoBlocked.cpp
    # `.adocao` 写入方（pack）：依赖 core 的 LevelData（archive → core 是允许的方向）
    AdocaoWriter.cpp
    # `.adocao` 读取方（unpack）：填 LevelData 但不做收尾（收尾与两条 JSON 路径共用）
    AdocaoReader.cpp
    # v3 的 `Derived` 段：每 4096 层的续算快照（§8.8 ⑧）。它要建一次 Timeline 才能拿到
    # timeSec/bpm —— **用权威实现，绝不重抄状态机**（所以这个文件依赖 core/timeline）。
    AdocaoCheckpoints.cpp
    # ③ 每砖结构窗口化：从 checkpoint + 一块 angleData + 这一块的事件重算窗口内的
    # Tile/tileBPMs/tileStartTimes（不碰全谱数组）。算式与浮点类型必须与生产路径逐字一致。
    AdocaoWindow.cpp
    # v3 的判据统计（`--codec-report` 的"为什么"）：纯诊断，不参与任何编码决策
    AdocaoDiagnostics.cpp
    # 资产包 `ADOCAO-data.tar.xz`（xz(tar(assets/…))）：解压 + tar 索引，供 core 的
    # DataFile 通过钩子读取。2026-10-25 起资产包不再用 zip（miniz 依赖已删）。
    AssetPack.cpp
    # Re_ADOJAS 的 `.ilytx`（xz(tar(ilybin))）：解 ilybin 的核心二进制
    Ilybin.cpp
    #   …… 再把它翻译成等价的 .adofai 明文，交回 core 那条唯一路径（不建第二条解析链）
    Ilytx.cpp
)

target_include_directories(adocao_archive PUBLIC ${CMAKE_SOURCE_DIR})
# 构建期注入 git commit（根 CMakeLists 里取；空 = 没有 git → 写入方写 20 个 0）
if(ADOCAO_GIT_COMMIT)
    target_compile_definitions(adocao_archive PRIVATE ADOCAO_GIT_COMMIT="${ADOCAO_GIT_COMMIT}")
endif()
# 头里已经没有任何 lzma/zstd 类型（pimpl，见 LevelArchive.hpp），所以它们在这里可以
# **真正 PRIVATE**：链 adocao_archive 的消费者不必配压缩库的头/库路径。
target_link_libraries(adocao_archive
    PUBLIC adocao_core
    PRIVATE liblzma libzstd_static
)
