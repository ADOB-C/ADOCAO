add_library(adocao_core STATIC
    level/LevelData.cpp
    level/LevelArchive.cpp
    level/LevelPath.cpp
    level/JsonCleaner.cpp
    util/Logger.cpp
    util/ThreadPool.cpp
    util/DataFile.cpp
    timeline/Timeline.cpp
    timeline/PositionSolver.cpp
    timeline/PlaybackClock.cpp
    map/LevelMap.cpp
    map/PngStream.cpp
)

target_include_directories(adocao_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(adocao_core
    PUBLIC glm::glm miniz liblzma libzstd_static
)

# RapidJSON is header-only; expose its include directory to consumers that use
# LevelData.hpp (which includes <rapidjson/document.h>).
target_include_directories(adocao_core PUBLIC ${rapidjson_SOURCE_DIR}/include)
