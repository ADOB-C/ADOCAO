cmake_minimum_required(VERSION 3.20)

# Prefer GCC over Clang (both over MSVC). Must be set before project().
if(NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER)
    find_program(GCC_C   gcc)
    find_program(GCC_CXX g++)
    find_program(CLANG_C   clang)
    find_program(CLANG_CXX clang++)
    if(GCC_C AND GCC_CXX)
        set(CMAKE_C_COMPILER   "${GCC_C}"   CACHE FILEPATH "C compiler")
        set(CMAKE_CXX_COMPILER "${GCC_CXX}" CACHE FILEPATH "C++ compiler")
    elseif(CLANG_C AND CLANG_CXX)
        set(CMAKE_C_COMPILER   "${CLANG_C}"   CACHE FILEPATH "C compiler")
        set(CMAKE_CXX_COMPILER "${CLANG_CXX}" CACHE FILEPATH "C++ compiler")
    endif()
endif()

project(ADOCAO VERSION 6.0.0 LANGUAGES CXX C)

option(ADOCAO_PORTABLE "Build a static-linked portable executable" OFF)

# ── Zoom level (min zoom-out distance) ─────────────────────────
set(ADOCAO_ZOOM_LEVEL "Ultra" CACHE STRING
    "Min zoom-out level: Normal(10) Extra(5) Super(2.5) Ultra(1) Hyper(0.5) Extreme(0.25) Unimaginable(0.1)")
set_property(CACHE ADOCAO_ZOOM_LEVEL PROPERTY STRINGS
    "Normal" "Extra" "Super" "Ultra" "Hyper" "Extreme" "Unimaginable")

if(ADOCAO_ZOOM_LEVEL STREQUAL "Normal")
    set(ADOCAO_MIN_ZOOM 10.0)
elseif(ADOCAO_ZOOM_LEVEL STREQUAL "Extra")
    set(ADOCAO_MIN_ZOOM 5.0)
elseif(ADOCAO_ZOOM_LEVEL STREQUAL "Super")
    set(ADOCAO_MIN_ZOOM 2.5)
elseif(ADOCAO_ZOOM_LEVEL STREQUAL "Ultra")
    set(ADOCAO_MIN_ZOOM 1.0)
elseif(ADOCAO_ZOOM_LEVEL STREQUAL "Hyper")
    set(ADOCAO_MIN_ZOOM 0.5)
elseif(ADOCAO_ZOOM_LEVEL STREQUAL "Extreme")
    set(ADOCAO_MIN_ZOOM 0.25)
elseif(ADOCAO_ZOOM_LEVEL STREQUAL "Unimaginable")
    set(ADOCAO_MIN_ZOOM 0.1)
else()
    message(WARNING "Unknown ADOCAO_ZOOM_LEVEL '${ADOCAO_ZOOM_LEVEL}', falling back to Ultra (1.0)")
    set(ADOCAO_MIN_ZOOM 1.0)
endif()
add_compile_definitions(ADOCAO_MIN_ZOOM=${ADOCAO_MIN_ZOOM}f)
message(STATUS "Zoom level: ${ADOCAO_ZOOM_LEVEL} (min ${ADOCAO_MIN_ZOOM}x)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Maximum safe optimization (no -ffast-math — timing precision is critical)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -fomit-frame-pointer)
endif()

# ---- Dependencies via FetchContent ----
include(FetchContent)

# rapidjson & stb are header-only with no usable CMake build (rapidjson's
# bundled CMake adds nested projects that fail as a subdirectory). Keep
# FetchContent_Populate for them; silence CMP0169 deprecation warning.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# glm
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

# RapidJSON (header-only, fast JSON parsing)
FetchContent_Declare(
    rapidjson
    GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(rapidjson)
if(NOT rapidjson_POPULATED)
    FetchContent_Populate(rapidjson)
endif()

# Dear ImGui
set(IMGUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# stb (header-only, just need the source for stb_vorbis)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

# miniaudio (single-header)
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.22
    GIT_SHALLOW TRUE
)
# Suppress miniaudio optional dependency not-found messages
set(MINIAUDIO_NO_EXTRA_NODES ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_LIBOPUS ON CACHE BOOL "" FORCE)
set(MINIAUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_LIBRARY "SDL2_LIBRARY-NOTFOUND" CACHE FILEPATH "" FORCE)
set(STEAMAUDIO_LIBRARY "STEAMAUDIO_LIBRARY-NOTFOUND" CACHE FILEPATH "" FORCE)
FetchContent_MakeAvailable(miniaudio)

# miniz (single-file zip library, public domain)
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(miniz)
target_include_directories(miniz PUBLIC ${miniz_SOURCE_DIR})

# tinyfiledialogs (single-header C library)
FetchContent_Declare(
    tinyfiledialogs
    GIT_REPOSITORY https://github.com/native-toolkit/tinyfiledialogs.git
    GIT_TAG 2.9.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tinyfiledialogs)

# ---- Library/application targets (directory = library boundary) ----
add_subdirectory(core)
add_subdirectory(glad)
add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(app)

# ---- Tests ----
enable_testing()
add_subdirectory(tests)
