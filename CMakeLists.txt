#
cmake_minimum_required(VERSION 3.21)

project(ucan-monitor VERSION 1.0 LANGUAGES C CXX)

set(upstream_dir ${PROJECT_SOURCE_DIR}/upstream)
if(NOT EXISTS ${upstream_dir}/include/imgui/imgui.h)
    message(FATAL_ERROR "upstream/ is not checked out: run 'sh win/checkout-upstream.sh' and 'sh win/patches/apply.sh'")
endif()

################################################################################
# Build tree laid out as upstream's: programs in bin/ next to assets/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(WIN32)
    file(COPY ${upstream_dir}/assets DESTINATION ${CMAKE_BINARY_DIR})
else()
    file(CREATE_LINK ${upstream_dir}/assets ${CMAKE_BINARY_DIR}/assets SYMBOLIC)
endif()

################################################################################
# Options all targets share
add_library(common_options INTERFACE)

target_compile_options(common_options INTERFACE
    $<$<CONFIG:Debug>:-Og -ggdb>
    $<$<CONFIG:Release>:-O3 -g0>
    -Wall
    -Wextra
    -Wpedantic
    -fdiagnostics-color=auto
)

target_compile_features(common_options INTERFACE
    c_std_11
    cxx_std_23
)

if(WIN32)
    # The CAN messages are structs with bit-fields copied byte for byte into
    # frames. MinGW lays bit-fields out the MSVC way by default; this flag
    # keeps the GCC layout the Linux build and the firmware use.
    target_compile_options(common_options INTERFACE -mno-ms-bitfields)

    # localtime_r() from the MinGW-w64 <time.h>
    target_compile_definitions(common_options INTERFACE
        _POSIX_THREAD_SAFE_FUNCTIONS=200112L
    )
endif()

################################################################################
# candle: the CAN backend, can::Socket over a candleLight adapter (libusb)
find_package(PkgConfig REQUIRED)
pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

file(GLOB_RECURSE candle_sources CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/win/src/cansocket/*.cpp
)
add_library(candle STATIC ${candle_sources})

target_include_directories(candle PUBLIC
    ${PROJECT_SOURCE_DIR}/win/include
    ${upstream_dir}/include # bsclog
)

target_link_libraries(candle PUBLIC
    common_options
    PkgConfig::libusb
)

if(WIN32)
    target_link_libraries(candle PUBLIC winmm) # timeBeginPeriod()
endif()

# The commits of this repository and of upstream, for the first line of the
# log; configuring again after a commit here updates them.
execute_process(COMMAND git -C ${PROJECT_SOURCE_DIR} rev-parse --short=7 HEAD
    OUTPUT_VARIABLE win_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C ${upstream_dir} rev-parse --short=7 HEAD
    OUTPUT_VARIABLE upstream_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C ${PROJECT_SOURCE_DIR} rev-parse --git-path HEAD
    OUTPUT_VARIABLE git_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(git_head)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/${git_head})
endif()
target_compile_definitions(candle PUBLIC
    UCAN_WIN_VERSION="${win_version}"
    UCAN_UPSTREAM_VERSION="${upstream_version}"
)

################################################################################
# Sources: all of upstream except its SocketCAN backend, which candle replaces
file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS
    ${upstream_dir}/src/*.c
    ${upstream_dir}/src/*.cpp
)
file(GLOB_RECURSE upstream_can_sources CONFIGURE_DEPENDS
    ${upstream_dir}/src/cansocket/*
)
list(REMOVE_ITEM app_sources ${upstream_can_sources})

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
    ${PROJECT_SOURCE_DIR}/win/src/app/working_directory.cpp
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
    common_options
    candle
    atomic
    glfw
    OpenGL::GL
)

if(WIN32)
    enable_language(RC)

    # The icon: upstream's PNG wrapped into an .ico at build time
    add_executable(png2ico ${PROJECT_SOURCE_DIR}/win/tools/png2ico.cpp)
    target_link_libraries(png2ico PRIVATE common_options)
    set(icon_png ${upstream_dir}/icons/hicolor/48x48/apps/ucan-monitor.png)
    set(icon_file ${CMAKE_BINARY_DIR}/ucan-monitor.ico)
    add_custom_command(OUTPUT ${icon_file}
        COMMAND png2ico ${icon_png} ${icon_file}
        DEPENDS png2ico ${icon_png}
        COMMENT "Making the program icon"
    )

    # Manifest: UTF-8 as the process code page, so that the UTF-8 paths ImGui
    # and the file dialog produce open correctly with Cyrillic in them.
    set(manifest_file ${PROJECT_SOURCE_DIR}/win/res/ucan-monitor.manifest)

    set(rc_file ${CMAKE_BINARY_DIR}/ucan-monitor.rc)
    configure_file(${PROJECT_SOURCE_DIR}/win/res/ucan-monitor.rc.in ${rc_file} @ONLY)
    set_source_files_properties(${rc_file} PROPERTIES
        OBJECT_DEPENDS "${icon_file};${manifest_file}"
    )
    target_sources(${executable} PRIVATE ${rc_file})

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
# candle-test: checks an adapter without the GUI
add_executable(candle-test ${PROJECT_SOURCE_DIR}/win/tools/candle-test.cpp)
target_link_libraries(candle-test PRIVATE candle)

################################################################################
# Tests
enable_testing()

add_executable(bittiming_test
    ${PROJECT_SOURCE_DIR}/win/tests/bittiming_test.cpp
    ${PROJECT_SOURCE_DIR}/win/src/cansocket/bittiming.cpp
)
target_link_libraries(bittiming_test PRIVATE common_options)
add_test(NAME bittiming COMMAND bittiming_test)

################################################################################
# Misc Options
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

################################################################################
# Install: bin/ next to assets/, the layout upstream expects. The program
# loads its fonts from ../assets relative to the working directory, which is
# bin/ when it is started from Explorer.
if(WIN32)
    # Ship the DLLs the programs import from the toolchain (GLFW, libusb, the
    # GCC runtime, libatomic), found by reading their import tables.
    # Installing fails if one cannot be found.
    get_filename_component(toolchain_bin_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
    install(TARGETS ${executable} candle-test
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
    install(TARGETS ${executable} candle-test
        RUNTIME DESTINATION bin
    )
endif()
if(WIN32)
    # double-click load self-test of the adapter, writes bin/selftest.txt
    install(FILES ${PROJECT_SOURCE_DIR}/win/tools/selftest.cmd DESTINATION bin)
endif()
install(DIRECTORY ${upstream_dir}/assets/
    DESTINATION assets
)
install(FILES ${upstream_dir}/LICENSE
    DESTINATION .
)
