#pragma once

#include <algorithm>
#include <array>

#include "media_types.hpp"

struct VideoRtpFragment {
    const uint8_t *data;
    size_t size;
    std::array<uint8_t, 3> prefix{};
    size_t prefix_size = 0;
    bool marker = false;
};

enum class VideoPacketization { Invalid, Single, Fragmented };

template <typename Emit>
VideoPacketization packetize_video_nal(VideoCodec codec, const uint8_t *nal, size_t len,
                                      size_t max_payload, bool marker, Emit emit) {
    const bool hevc = codec == VideoCodec::H265;
    const size_t nal_header_size = hevc ? 2 : 1;
    const size_t fragment_header_size = nal_header_size + 1;
    if (!nal || len < nal_header_size || max_payload <= fragment_header_size) {
        return VideoPacketization::Invalid;
    }
    if (len <= max_payload) {
        emit(VideoRtpFragment{nal, len, {}, 0, marker});
        return VideoPacketization::Single;
    }

    const uint8_t type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
    std::array<uint8_t, 3> prefix{};
    if (hevc) {
        prefix[0] = (nal[0] & 0x81) | (49 << 1);
        prefix[1] = nal[1];
    } else {
        prefix[0] = (nal[0] & 0xe0) | 28;
    }

    size_t pos = nal_header_size;
    while (pos < len) {
        const size_t size = std::min(max_payload - fragment_header_size, len - pos);
        const bool first = pos == nal_header_size;
        const bool last = size == len - pos;
        prefix[nal_header_size] = (first ? 0x80 : 0) | (last ? 0x40 : 0) | type;
        emit(VideoRtpFragment{nal + pos, size, prefix, fragment_header_size, marker && last});
        pos += size;
    }
    return VideoPacketization::Fragmented;
}
