# archive 模块：谱面容器（.adofai.xz / .adofai.zst）的解压实现。
# 依赖倒置：core 只定义 ByteSource 接口，本模块实现并通过 adofai::archive::install() 注册。
# lzma/zstd 只挂在这里 —— 只想解析明文谱的消费者链 core 即可，不用背压缩库。
add_library(adocao_archive STATIC
    LevelArchive.cpp
)

target_include_directories(adocao_archive PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_archive
    PUBLIC adocao_core
    PRIVATE liblzma libzstd_static
)
