#pragma once

#include <vector>

#include "media_types.hpp"

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
    Impl *impl_ = nullptr;
};
