add_library(adocao_core STATIC
    level/LevelData.cpp
    level/LevelArchive.cpp
    level/LevelPath.cpp
    level/JsonCleaner.cpp
    util/AssetPaths.cpp
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

# 分带并行 PNG 写入器**必须**用 zlib：miniz 的 mz_deflate/mz_inflate 在这条用法下会吞行、
# 也无法解开自己写的流，拿它跑出来的 PNG 是坏的（而坏 PNG 比编译失败糟糕得多）。
# 系统没有就自己取一份 —— 之前"退回 miniz"的写法会让 Windows 便携版静默产出坏图。
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE
    )
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)          # 提供 zlib（共享）/ zlibstatic（静态）
endif()
target_compile_definitions(adocao_core PRIVATE ADOCAO_HAVE_ZLIB=1)
if(ZLIB_FOUND)
    # 只链库，绝不引入它的 include 目录：macOS 上 ZLIB::ZLIB 会带上 /usr/include，
    # 那会打乱 libc++ 的标准头顺序（报 <cstddef> 找不到 <stddef.h>）。<zlib.h> 本来就在默认路径。
    target_link_libraries(adocao_core PUBLIC ${ZLIB_LIBRARIES})
    message(STATUS "PngBand: 使用系统 zlib")
else()
    target_link_libraries(adocao_core PUBLIC zlibstatic)
    message(STATUS "PngBand: 系统没有 zlib，已 FetchContent 取一份")
endif()

target_include_directories(adocao_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_core
    PUBLIC glm::glm miniz liblzma libzstd_static
)

# RapidJSON is header-only; expose its include directory to consumers that use
# LevelData.hpp (which includes <rapidjson/document.h>).
target_include_directories(adocao_core PUBLIC ${rapidjson_SOURCE_DIR}/include)
