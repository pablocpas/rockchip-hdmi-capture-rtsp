#pragma once

#include "media_types.hpp"
#include "options.hpp"

std::unique_ptr<MediaOutput> make_rtsp_publisher(const Options &opt, const std::vector<uint8_t> &header);
