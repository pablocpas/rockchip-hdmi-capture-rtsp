#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "rk_mpi.h"

struct CapturedFrame {
    const uint8_t *data = nullptr;
    size_t bytesused = 0;
    size_t buffer_length = 0;
    size_t bytesperline = 0;
    uint64_t timestamp_ns = 0;
    int dmabuf_fd = -1;
    MppBuffer mpp_buffer = nullptr;
    unsigned index = 0;
};

struct RgaConverterConfig {
    std::string library_path;
    int width = 0;
    int height = 0;
    int src_stride_bytes = 0;
    int dst_hor_stride = 0;
    int dst_ver_stride = 0;
    int dst_fd = -1;
    size_t dst_size = 0;
};

struct RgaScalerConfig {
    std::string library_path;
    int dst_width = 0;
    int dst_height = 0;
    int dst_hor_stride = 0;
    int dst_ver_stride = 0;
    int dst_fd = -1;
};
