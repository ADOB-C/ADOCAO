add_library(adocao_core STATIC
    level/LevelData.cpp
    level/LevelArchive.cpp
    level/LevelPath.cpp
    level/JsonCleaner.cpp
    util/Logger.cpp
    util/Progress.cpp
    util/ThreadPool.cpp
    util/DataFile.cpp
    timeline/Timeline.cpp
    timeline/PositionSolver.cpp
    timeline/PlaybackClock.cpp
    map/LevelMap.cpp
    map/PngStream.cpp
    map/PngBand.cpp
)

# 分带并行 PNG 写入器优先用系统 zlib（miniz 的 mz_deflate 在该用法下会卡死）；
# 找不到就退回 miniz —— 两边的 API 由 PngBand.cpp 顶部的名称垫片统一。
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_compile_definitions(adocao_core PRIVATE ADOCAO_HAVE_ZLIB=1)
    # 只链库，绝不引入它的 include 目录：macOS 上 ZLIB::ZLIB 会带上 /usr/include，
    # 那会打乱 libc++ 的标准头顺序（报 <cstddef> 找不到 <stddef.h>）。<zlib.h> 本来就在默认路径。
    target_link_libraries(adocao_core PUBLIC ${ZLIB_LIBRARIES})
    message(STATUS "PngBand: 使用系统 zlib")
else()
    message(STATUS "PngBand: 未找到 zlib，退回 miniz")
endif()

target_include_directories(adocao_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_core
    PUBLIC glm::glm miniz liblzma libzstd_static
)

# RapidJSON is header-only; expose its include directory to consumers that use
# LevelData.hpp (which includes <rapidjson/document.h>).
target_include_directories(adocao_core PUBLIC ${rapidjson_SOURCE_DIR}/include)
