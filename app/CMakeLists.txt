# App/UI/executable. This is the only target that may depend on GLFW/ImGui and
# the only target allowed to create a window.
set(ADOCAO_APP_SOURCES
    main.cpp
    Application.cpp
    AssetSetup.cpp
    PlatformPaths.cpp
    Application.hpp
    LauncherWindow.cpp
    LauncherWindow.hpp
    LoadingWindow.cpp
    LoadingWindow.hpp
    FileMap.hpp
    LevelLoader.cpp
    MapExport.cpp
    LevelLoader.hpp
    GameWindow.cpp
    GameWindow.hpp
    CameraController.cpp
    CameraController.hpp
    LevelScene.cpp
    LevelScene.hpp
    wizard/WizardState.hpp
    wizard/WizardChrome.cpp
    wizard/WizardChrome.hpp
    wizard/FileDialogs.cpp
    wizard/FileDialogs.hpp
    wizard/PageWelcome.cpp
    wizard/PageHitsounds.cpp
    wizard/PageGraphics.cpp
    wizard/PageTrail.cpp
    wizard/PageColors.cpp
    wizard/PageMusic.cpp
)

add_library(adocao_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(adocao_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(adocao_imgui PUBLIC glfw)

# tinyfiledialogs (single-header C library, no upstream CMake target)
add_library(tinyfiledialogs STATIC ${tinyfiledialogs_SOURCE_DIR}/tinyfiledialogs.c)
target_include_directories(tinyfiledialogs PUBLIC ${tinyfiledialogs_SOURCE_DIR})


if(APPLE)
    # macOS 专用：真正的 borderless fullscreen（抬高 window level + 隐藏菜单栏/Dock）。
    # 注意 OBJCXX 必须在**顶层** project() 之后 enable（见根 CMakeLists）—— 在这里 enable
    # 会让已经配置过的 GLFW 目标丢掉归档规则（link.txt 变空，libglfw3.a 永远不生成）。
    list(APPEND ADOCAO_APP_SOURCES MacBorderless.mm MacBorderless.hpp)
endif()
add_executable(adocao ${ADOCAO_APP_SOURCES})
set_target_properties(adocao PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    set_target_properties(adocao PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "ADOCAO")
else()
    set_target_properties(adocao PROPERTIES OUTPUT_NAME "ADOCAO")
endif()

target_include_directories(adocao PRIVATE ${CMAKE_SOURCE_DIR} ${stb_SOURCE_DIR})
target_link_libraries(adocao PRIVATE
    adocao_core
    adocao_archive
    adocao_render
    adocao_audio
    adocao_glad
    adocao_imgui
    glfw
    glm::glm
    tinyfiledialogs
)

# Platform-specific
if(WIN32)
    target_link_libraries(adocao PRIVATE opengl32 ole32 shell32)
    target_compile_definitions(adocao PRIVATE NOMINMAX UNICODE _UNICODE)
elseif(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(adocao PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(WAYLAND wayland-client wayland-egl IMPORTED_TARGET QUIET)
    endif()
    if(WAYLAND_FOUND)
        message(STATUS "Wayland found — GLFW will support Wayland")
    endif()
elseif(APPLE)
    target_compile_definitions(adocao PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(adocao PRIVATE
        "-framework OpenGL"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
endif()

# Strip Release binaries
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    if(MINGW)
        target_link_options(adocao PRIVATE -Wl,-s)
    elseif(MSVC)
        target_link_options(adocao PRIVATE /OPT:REF /OPT:ICF)
    endif()
endif()

# Static linking for portable builds
if(ADOCAO_PORTABLE)
    if(MINGW)
        target_link_options(adocao PRIVATE -static)
    elseif(MSVC)
        set_property(TARGET adocao PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# ---- Install ----
install(TARGETS adocao RUNTIME DESTINATION bin)

# ---- Copy assets (hitsounds, shaders) and MinGW DLLs to build directory ----
add_custom_command(TARGET adocao POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${PROJECT_SOURCE_DIR}/assets/hitsounds"
    "${PROJECT_BINARY_DIR}/assets/hitsounds"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${PROJECT_SOURCE_DIR}/assets/shaders"
    "${PROJECT_BINARY_DIR}/assets/shaders"
    COMMENT "Copying assets (hitsounds, shaders) to build directory"
)

# Copy MinGW runtime DLLs next to EXE
if(MINGW)
    # 运行时 DLL 不在编译器旁边也能找到：直接问编译器（`-print-file-name=` 返回 sysroot 里的路径）。
    # 旧实现假设 `${CMAKE_CXX_COMPILER}` 同目录有 DLL —— Homebrew 的 mingw 放在
    # `toolchain-x86_64/x86_64-w64-mingw32/lib/`，于是 POST_BUILD 报"复制失败"。
    set(MINGW_DLLS "")
    foreach(dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${dll}
                        OUTPUT_VARIABLE _p OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(EXISTS "${_p}")
            list(APPEND MINGW_DLLS "${_p}")
        endif()
    endforeach()
    if(NOT MINGW_DLLS)
        get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        list(APPEND MINGW_DLLS
            "${MINGW_BIN}/libgcc_s_dw2-1.dll"
            "${MINGW_BIN}/libstdc++-6.dll"
            "${MINGW_BIN}/libwinpthread-1.dll")
    endif()
    add_custom_command(TARGET adocao POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MINGW_DLLS} "${PROJECT_BINARY_DIR}"
        COMMENT "Copying MinGW runtime DLLs to build directory"
    )
endif()

# ---- Copy example level (if available) ----
if(EXISTS "${PROJECT_SOURCE_DIR}/../re_adojas/lib/example/level.adofai")
    file(COPY ${PROJECT_SOURCE_DIR}/../re_adojas/lib/example/level.adofai
         DESTINATION ${PROJECT_BINARY_DIR})
endif()
