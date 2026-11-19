#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "media_types.hpp"

class RtspServer {
public:
    struct StreamProfile {
        std::string name;
        std::string path;
        bool video = true;
        VideoCodec video_codec = VideoCodec::H264;
        std::function<std::vector<uint8_t>()> video_header_provider;
        int fps = 30;
        bool audio = false;
        std::string audio_codec;
        size_t audio_frame_frames = 960;
        int send_timeout_ms = 2000;
    };

    struct StreamSelection {
        int profile_index = -1;
        uint64_t generation = 0;

        bool operator==(const StreamSelection &other) const {
            return profile_index == other.profile_index && generation == other.generation;
        }
        bool operator!=(const StreamSelection &other) const { return !(*this == other); }
        explicit operator bool() const { return profile_index >= 0; }
    };

    RtspServer(const std::string &listen_addr,
               std::vector<StreamProfile> profiles,
               int rtp_payload_size, bool rtsp_debug, int max_clients);
    ~RtspServer();

    uint16_t port() const;
    StreamSelection stream_selection() const;
    std::vector<StreamSelection> active_streams() const;
    bool wait_for_clients(std::chrono::milliseconds timeout);
    bool consume_keyframe_request();
    bool consume_keyframe_request(int profile_index);
    void request_keyframe();
    void request_keyframe(int profile_index);
    void invalidate_video(StreamSelection selection);
    void set_video_header(StreamSelection selection, const std::vector<uint8_t> &header);

    void log_stats();

    void write_video(const EncodedPacket &packet, StreamSelection selection);
    void write_audio_l16(const int16_t *samples, size_t frames, StreamSelection selection);
    void write_audio_opus(const uint8_t *payload, size_t len, size_t frames, StreamSelection selection);
    void write_audio_l16_all(const int16_t *samples, size_t frames);
    void write_audio_opus_all(const uint8_t *payload, size_t len, size_t frames);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
