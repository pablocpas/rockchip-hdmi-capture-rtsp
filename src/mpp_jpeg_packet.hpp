#pragma once

#include <cstddef>
#include <memory>

#include "rk_mpi.h"

class MppJpegPacket {
public:
    explicit MppJpegPacket(MppBuffer buffer);
    MppPacket prepare(size_t jpeg_size);

private:
    struct Deleter {
        void operator()(MppPacket packet) const;
    };

    std::unique_ptr<void, Deleter> packet_;
    void *data_ = nullptr;
    size_t capacity_ = 0;
};
