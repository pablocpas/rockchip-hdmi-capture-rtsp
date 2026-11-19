#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rk_mpi.h"

std::vector<uint8_t> read_mpp_video_header(MppCtx ctx, const MppApi &api, size_t buffer_size);
