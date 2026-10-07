# archive 模块：谱面容器（.adofai.xz / .adofai.zst）的解压实现。
# 依赖倒置：core 只定义 ByteSource 接口，本模块实现并通过 adofai::archive::install() 注册。
# lzma/zstd 只挂在这里 —— 只想解析明文谱的消费者链 core 即可，不用背压缩库。
add_library(adocao_archive STATIC
    LevelArchive.cpp
    # `.adocao` 的列编码器（字典/常量/差分/位打包/原始）。它**不依赖** lzma/zstd：
    # 压缩只是列流之后的可选一步，设计核心是"每列按取值基数选编码"。
    AdocaoColumns.cpp
)

target_include_directories(adocao_archive PUBLIC ${CMAKE_SOURCE_DIR})
# 头里已经没有任何 lzma/zstd 类型（pimpl，见 LevelArchive.hpp），所以它们在这里可以
# **真正 PRIVATE**：链 adocao_archive 的消费者不必配压缩库的头/库路径。
target_link_libraries(adocao_archive
    PUBLIC adocao_core
    PRIVATE liblzma libzstd_static
)
