#pragma once

#include <cstddef>
#include <cstdint>

// Only the MPP calls used by src/mpp_header.cpp are simulated.
using MppCtx = void *;
using MppBufferGroup = void *;
using MppBuffer = void *;
using MppPacket = void *;
using MppParam = void *;
using MPP_RET = int;

enum MpiCmd { MPP_ENC_GET_HDR_SYNC };

struct MppApi {
    MPP_RET (*control)(MppCtx, MpiCmd, MppParam);
};

constexpr int MPP_BUFFER_TYPE_DRM = 1;
constexpr int MPP_BUFFER_FLAGS_CACHABLE = 0x00020000;

MPP_RET mpp_buffer_group_get_internal(MppBufferGroup *, int);
MPP_RET mpp_buffer_group_put(MppBufferGroup);
MPP_RET mpp_buffer_get(MppBufferGroup, MppBuffer *, size_t);
MPP_RET mpp_buffer_put(MppBuffer);
MPP_RET mpp_packet_init_with_buffer(MppPacket *, MppBuffer);
MPP_RET mpp_packet_deinit(MppPacket *);
void mpp_packet_set_pos(MppPacket, void *);
void mpp_packet_set_size(MppPacket, size_t);
void mpp_packet_set_flag(MppPacket, uint32_t);
void *mpp_packet_get_data(MppPacket);
size_t mpp_packet_get_size(MppPacket);
void mpp_packet_set_length(MppPacket, size_t);
void *mpp_packet_get_pos(MppPacket);
size_t mpp_packet_get_length(MppPacket);
