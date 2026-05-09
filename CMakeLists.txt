cmake_minimum_required(VERSION 3.16)
project(rockchip_hdmi_capture_rtsp LANGUAGES CXX)

include(GNUInstallDirs)
include(CheckIncludeFileCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(RGA_ROOT "" CACHE PATH "Optional librga checkout or install prefix")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBV4L2 REQUIRED libv4l2)
pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)
pkg_check_modules(ROCKCHIP_MPP REQUIRED rockchip_mpp)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(OPUS opus)
check_include_file_cxx(libyuv/planar_functions.h HAVE_LIBYUV_HEADER)
find_library(LIBYUV_LIBRARY NAMES yuv libyuv)
find_path(RGA_INCLUDE_DIR
    NAMES im2d.h
    HINTS
        ${RGA_ROOT}/include
        $ENV{LIBRGA_ROOT}/include
    PATHS
        /usr/local/include/rga
        /usr/local/include
)

add_executable(rk-hdmi-streamer
    src/h264_utils.cpp
    src/main.cpp
    src/rga_converter.cpp
)

target_include_directories(rk-hdmi-streamer PRIVATE
    ${LIBV4L2_INCLUDE_DIRS}
    ${TURBOJPEG_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${OPUS_INCLUDE_DIRS}
    /usr/include/rockchip
)

target_link_libraries(rk-hdmi-streamer PRIVATE
    ${LIBV4L2_LIBRARIES}
    ${TURBOJPEG_LIBRARIES}
    ${ROCKCHIP_MPP_LIBRARIES}
    ${ALSA_LIBRARIES}
    ${OPUS_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

if(OPUS_FOUND)
    target_compile_definitions(rk-hdmi-streamer PRIVATE HAVE_OPUS=1)
endif()

if(HAVE_LIBYUV_HEADER AND LIBYUV_LIBRARY)
    target_link_libraries(rk-hdmi-streamer PRIVATE ${LIBYUV_LIBRARY})
    target_compile_definitions(rk-hdmi-streamer PRIVATE HAVE_LIBYUV=1)
endif()

if(RGA_INCLUDE_DIR)
    target_include_directories(rk-hdmi-streamer PRIVATE ${RGA_INCLUDE_DIR})
    target_compile_definitions(rk-hdmi-streamer PRIVATE HAVE_RGA=1)
endif()

target_compile_options(rk-hdmi-streamer PRIVATE -Wall -Wextra -Wpedantic -O2)

install(TARGETS rk-hdmi-streamer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
