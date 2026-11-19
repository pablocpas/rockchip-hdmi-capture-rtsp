#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "media_types.hpp"
#include "options.hpp"

struct CaptureStats {
    uint64_t captured = 0;
    uint64_t skipped = 0;
};

class StreamPipeline {
public:
    using PacketWriter = std::function<void(EncodedPacket, uint64_t)>;
    using MultiPacketWriter = std::function<void(int, EncodedPacket, uint64_t)>;

    explicit StreamPipeline(const Options &capture_opt, bool enable_default_encoder = true);
    ~StreamPipeline();

    const std::vector<uint8_t> &video_header() const;
    const std::vector<uint8_t> &enable_stream(int stream_id, const Options &stream_opt);
    void disable_stream(int stream_id);
    bool has_stream(int stream_id) const;

    void start();
    void stop();
    void request_idr();
    void request_idr(int stream_id);
    bool read_frame(const PacketWriter &writer, CaptureStats &stats);
    bool read_frame_multi(const MultiPacketWriter &writer, CaptureStats &stats);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<uint8_t> make_video_header(const Options &opt);
