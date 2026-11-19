#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

enum class VideoCodec {
    H264,
    H265,
};

struct NalUnit {
    const uint8_t *data = nullptr;
    size_t size = 0;
};

struct EncodedPacket {
    const uint8_t *data = nullptr;
    size_t len = 0;
    std::shared_ptr<void> owner; // Retains packet storage for asynchronous sends.
    uint32_t rtp_timestamp = 0;
    bool has_rtp_timestamp = false;
    bool (*nal_provider)(const EncodedPacket &, std::vector<NalUnit> &) = nullptr;
    void *nal_ctx = nullptr; // Non-owning context valid during synchronous write.
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual void write_audio_l16_s16le(const int16_t *samples, size_t frames) = 0;
    virtual void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) = 0;
};

class MediaOutput : public AudioOutput {
public:
    virtual void write_packet(const EncodedPacket &packet) = 0;
    virtual void log_stats() = 0;
};
