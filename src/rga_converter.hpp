#pragma once

#include <memory>
#include <vector>

#include "capture_types.hpp"

class RgaYuyvToNv12Converter {
public:
    RgaYuyvToNv12Converter(const RgaConverterConfig &cfg,
                           const std::vector<MppBuffer> &src_buffers);
    ~RgaYuyvToNv12Converter();

    RgaYuyvToNv12Converter(const RgaYuyvToNv12Converter &) = delete;
    RgaYuyvToNv12Converter &operator=(const RgaYuyvToNv12Converter &) = delete;

    void convert(const CapturedFrame &frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class RgaNv12Scaler {
public:
    explicit RgaNv12Scaler(const RgaScalerConfig &cfg);
    ~RgaNv12Scaler();

    RgaNv12Scaler(const RgaNv12Scaler &) = delete;
    RgaNv12Scaler &operator=(const RgaNv12Scaler &) = delete;

    void scale(int src_fd, int src_width, int src_height, int src_hor_stride, int src_ver_stride);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
