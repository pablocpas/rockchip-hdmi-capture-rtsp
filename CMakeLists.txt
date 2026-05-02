cmake_minimum_required(VERSION 3.16)
project(rockchip_hdmi_capture_rtsp LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBV4L2 REQUIRED libv4l2)
pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)
pkg_check_modules(ROCKCHIP_MPP REQUIRED rockchip_mpp)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(OPUS opus)

add_executable(rk-hdmi-streamer
    src/main.cpp
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
)

if(OPUS_FOUND)
    target_compile_definitions(rk-hdmi-streamer PRIVATE HAVE_OPUS=1)
endif()

target_compile_options(rk-hdmi-streamer PRIVATE -Wall -Wextra -Wpedantic -O2)

install(TARGETS rk-hdmi-streamer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
