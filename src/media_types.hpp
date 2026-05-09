#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <type_traits>

#include "rk_mpi.h"

struct MppPacketDeleter {
    void operator()(std::remove_pointer<MppPacket>::type *pkt) const {
        if (pkt) mpp_packet_deinit(&pkt);
    }
};

using SharedMppPacket = std::shared_ptr<std::remove_pointer<MppPacket>::type>;

enum class VideoCodec {
    H264,
    H265,
};

struct EncodedPacket {
    const uint8_t *data = nullptr;
    size_t len = 0;
    SharedMppPacket mpp_packet;
    uint32_t rtp_timestamp = 0;
    bool has_rtp_timestamp = false;
};

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

class MediaOutput {
public:
    virtual ~MediaOutput() = default;
    virtual void write_frame(const uint8_t *data, size_t len) = 0;
    virtual void write_packet(const EncodedPacket &packet) { write_frame(packet.data, packet.len); }
    virtual void write_audio_l16_s16le(const int16_t *samples, size_t frames) = 0;
    virtual void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) = 0;
    virtual void log_stats() = 0;
};
