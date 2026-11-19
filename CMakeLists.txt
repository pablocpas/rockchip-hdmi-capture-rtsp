cmake_minimum_required(VERSION 3.16)
project(rockchip_hdmi_capture_rtsp LANGUAGES CXX)

include(CheckIPOSupported)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(RK_BUILD_STREAMER "Build the Rockchip capture executable" ON)
option(RK_ENABLE_LTO "Enable Link-Time Optimization for Release builds" ON)
set(RK_CPU "" CACHE STRING "Optional target CPU for -mcpu (e.g. cortex-a55 or native)")
set(RGA_ROOT "" CACHE PATH "Optional librga checkout or install prefix")

if(RK_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    check_ipo_supported(RESULT RK_IPO_SUPPORTED OUTPUT RK_IPO_ERROR)
    if(RK_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

set(RK_CPU_FLAGS "")
if(RK_CPU)
    list(APPEND RK_CPU_FLAGS "-mcpu=${RK_CPU}")
endif()

find_package(Threads REQUIRED)

add_library(rk-stream-core STATIC
    src/h264_utils.cpp
    src/options.cpp
    src/rtsp_server.cpp
)
target_include_directories(rk-stream-core PUBLIC src)
target_link_libraries(rk-stream-core PUBLIC Threads::Threads)
target_compile_options(rk-stream-core PRIVATE -Wall -Wextra -Wpedantic -fno-rtti $<$<CONFIG:Release>:-O3> ${RK_CPU_FLAGS})

if(RK_BUILD_STREAMER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ROCKCHIP_MPP REQUIRED IMPORTED_TARGET rockchip_mpp)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    find_path(ROCKCHIP_MPP_HEADER_DIR NAMES rk_mpi.h
        HINTS ${ROCKCHIP_MPP_INCLUDE_DIRS}
        PATH_SUFFIXES rockchip
    )
    if(NOT ROCKCHIP_MPP_HEADER_DIR)
        message(FATAL_ERROR "Rockchip MPP headers not found")
    endif()
    pkg_check_modules(OPUS QUIET IMPORTED_TARGET opus)
    find_path(LIBYUV_INCLUDE_DIR NAMES libyuv/planar_functions.h)
    find_library(LIBYUV_LIBRARY NAMES yuv libyuv)
    find_path(RGA_INCLUDE_DIR NAMES im2d.h
        HINTS ${RGA_ROOT}/include $ENV{LIBRGA_ROOT}/include
        PATH_SUFFIXES rga
    )

    add_executable(rk-hdmi-streamer
        src/audio_capture.cpp
        src/main.cpp
        src/mpp_header.cpp
        src/mpp_jpeg_packet.cpp
        src/rga_converter.cpp
        src/rtsp_publisher.cpp
        src/stream_pipeline.cpp
    )
    target_link_libraries(rk-hdmi-streamer PRIVATE
        rk-stream-core
        PkgConfig::ROCKCHIP_MPP
        PkgConfig::ALSA
        ${CMAKE_DL_LIBS}
    )
    target_include_directories(rk-hdmi-streamer SYSTEM PRIVATE ${ROCKCHIP_MPP_HEADER_DIR})
    target_compile_options(rk-hdmi-streamer PRIVATE -Wall -Wextra -Wpedantic -fno-rtti $<$<CONFIG:Release>:-O3> ${RK_CPU_FLAGS})

    if(OPUS_FOUND)
        target_link_libraries(rk-hdmi-streamer PRIVATE PkgConfig::OPUS)
        target_compile_definitions(rk-hdmi-streamer PRIVATE HAVE_OPUS=1)
        target_compile_definitions(rk-stream-core PRIVATE HAVE_OPUS=1)
    endif()

    if(LIBYUV_INCLUDE_DIR AND LIBYUV_LIBRARY)
        target_include_directories(rk-hdmi-streamer PRIVATE ${LIBYUV_INCLUDE_DIR})
        target_link_libraries(rk-hdmi-streamer PRIVATE ${LIBYUV_LIBRARY})
        target_compile_definitions(rk-hdmi-streamer PRIVATE HAVE_LIBYUV=1)
    endif()

    if(RGA_INCLUDE_DIR)
        target_include_directories(rk-hdmi-streamer SYSTEM PRIVATE ${RGA_INCLUDE_DIR})
        target_compile_definitions(rk-hdmi-streamer PRIVATE HAVE_RGA=1)
        target_compile_definitions(rk-stream-core PRIVATE HAVE_RGA=1)
    endif()

    install(TARGETS rk-hdmi-streamer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES man/rk-hdmi-streamer.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
    install(FILES completions/rk-hdmi-streamer.bash
            DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
            RENAME rk-hdmi-streamer)
endif()

if(BUILD_TESTING)
    set(RK_STRESS_SECONDS 0 CACHE STRING "Duration of the optional RTSP stress test")
    add_executable(options-test tests/options_test.cpp)
    target_link_libraries(options-test PRIVATE rk-stream-core)
    add_test(NAME options COMMAND options-test)

    add_executable(rtsp-test tests/rtsp_test.cpp)
    target_link_libraries(rtsp-test PRIVATE rk-stream-core)
    add_test(NAME rtsp COMMAND rtsp-test)

    add_executable(mpp-header-test tests/mpp_header_test.cpp src/mpp_header.cpp)
    target_include_directories(mpp-header-test PRIVATE src tests/fakes)
    target_compile_options(mpp-header-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mpp-header COMMAND mpp-header-test)

    add_executable(mpp-jpeg-packet-test tests/mpp_jpeg_packet_test.cpp src/mpp_jpeg_packet.cpp)
    target_include_directories(mpp-jpeg-packet-test PRIVATE src tests/fakes)
    target_compile_options(mpp-jpeg-packet-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mpp-jpeg-packet COMMAND mpp-jpeg-packet-test)

    add_test(NAME configure COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/configure_test.sh)
    set_tests_properties(options rtsp mpp-header mpp-jpeg-packet configure PROPERTIES TIMEOUT 20)

    if(RK_STRESS_SECONDS GREATER 0)
        if(RK_STRESS_SECONDS GREATER 86400)
            message(FATAL_ERROR "RK_STRESS_SECONDS must be at most 86400")
        endif()
        math(EXPR stress_timeout "${RK_STRESS_SECONDS} + 30")
        add_test(NAME rtsp-stress COMMAND rtsp-test --stress ${RK_STRESS_SECONDS})
        set_tests_properties(rtsp-stress PROPERTIES TIMEOUT ${stress_timeout} LABELS stress)
    endif()
endif()
