#
cmake_minimum_required(VERSION 3.21)

project(ucan-monitor VERSION 1.0 LANGUAGES C CXX)

set(upstream_dir ${PROJECT_SOURCE_DIR}/upstream)
if(NOT EXISTS ${upstream_dir}/include/imgui/imgui.h)
    message(FATAL_ERROR "upstream/ is not checked out: run 'git submodule update --init --recursive'")
endif()

################################################################################
# Sources: all of upstream except its SocketCAN backend, which win/ replaces
file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS
    ${upstream_dir}/src/*.c
    ${upstream_dir}/src/*.cpp
)
file(GLOB_RECURSE upstream_can_sources CONFIGURE_DEPENDS
    ${upstream_dir}/src/cansocket/*
)
list(REMOVE_ITEM app_sources ${upstream_can_sources})

file(GLOB_RECURSE win_sources CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/win/src/*.cpp
)

################################################################################
# Dear ImGui, as upstream builds it
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

file(GLOB imgui_sources CONFIGURE_DEPENDS
    ${upstream_dir}/include/imgui/*.cpp
    ${upstream_dir}/include/implot/*.cpp
    ${upstream_dir}/include/imguifiledialog/*.cpp
)
file(GLOB imgui_backend_sources CONFIGURE_DEPENDS
    ${upstream_dir}/include/imgui/backends/*glfw*.cpp
    ${upstream_dir}/include/imgui/backends/*opengl3*.cpp
)

################################################################################
# Executable
set(executable ${CMAKE_PROJECT_NAME})

add_executable(${executable}
    ${app_sources}
    ${win_sources}
    ${imgui_sources}
    ${imgui_backend_sources}
)

# win/include goes first: upstream's #include <linux/can.h> and
# #include <cansocket/cansocket.h> must resolve to the headers there.
target_include_directories(${executable} PRIVATE
    ${PROJECT_SOURCE_DIR}/win/include
    ${upstream_dir}
    ${upstream_dir}/include
    ${upstream_dir}/src
    ${upstream_dir}/include/imgui
    ${upstream_dir}/include/implot
)

target_compile_definitions(${executable} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    IMGUI_USE_WCHAR32
)

target_link_libraries(${executable} PRIVATE
    atomic
    glfw
    OpenGL::GL
)

target_compile_options(${executable} PRIVATE
    $<$<CONFIG:Debug>:-Og -ggdb>
    $<$<CONFIG:Release>:-O3 -g0>
    -Wall
    -Wextra
    -Wpedantic
    -fdiagnostics-color=auto
)

target_compile_features(${executable} PRIVATE
    c_std_11
    cxx_std_23
)

################################################################################
# Windows
if(WIN32)
    enable_language(RC)

    # The CAN messages are structs with bit-fields copied byte for byte into
    # frames. MinGW lays bit-fields out the MSVC way by default; this flag
    # keeps the GCC layout the Linux build and the firmware use.
    target_compile_options(${executable} PRIVATE -mno-ms-bitfields)

    # localtime_r() from the MinGW-w64 <time.h>
    target_compile_definitions(${executable} PRIVATE
        _POSIX_THREAD_SAFE_FUNCTIONS=200112L
    )

    # Manifest: UTF-8 as the process code page, so that the UTF-8 paths ImGui
    # and the file dialog produce open correctly with Cyrillic in them.
    target_sources(${executable} PRIVATE ${PROJECT_SOURCE_DIR}/win/res/ucan-monitor.rc)

    # A GUI program: no console window next to it. The log still goes to log.txt.
    set_target_properties(${executable} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

################################################################################
# Git Version Tracking: report the upstream commit, not this repository's
set(GIT_WORKING_DIR ${upstream_dir})
add_subdirectory(${upstream_dir}/cmake/git-version-tracking
                 ${CMAKE_BINARY_DIR}/cmake/git-version-tracking)

target_link_libraries(${executable} PRIVATE
    git-version-tracking
)

################################################################################
# Misc Options
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

################################################################################
# Install: bin/ next to assets/, the layout upstream expects. The program
# loads its fonts from ../assets relative to the working directory, which is
# bin/ when it is started from Explorer.
if(WIN32)
    # Ship the DLLs the program imports from the toolchain (GLFW, the GCC
    # runtime, libatomic), found by reading the executable's import table.
    # Installing fails if one cannot be found.
    get_filename_component(toolchain_bin_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
    install(TARGETS ${executable}
        RUNTIME_DEPENDENCY_SET runtime_dlls
        RUNTIME DESTINATION bin
    )
    # The search options must come before RUNTIME DESTINATION
    install(RUNTIME_DEPENDENCY_SET runtime_dlls
        PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
        POST_EXCLUDE_REGEXES "[Ss][Yy][Ss][Tt][Ee][Mm]32/"
        DIRECTORIES ${toolchain_bin_dir}
        RUNTIME DESTINATION bin
    )
else()
    install(TARGETS ${executable}
        RUNTIME DESTINATION bin
    )
endif()
install(DIRECTORY ${upstream_dir}/assets/
    DESTINATION assets
)
install(FILES ${upstream_dir}/LICENSE
    DESTINATION .
)
