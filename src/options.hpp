#pragma once

#include <cstddef>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string device = "/dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0";
    std::string audio_device = "hw:CARD=Video,DEV=0";
    std::string audio_codec = "l16";
    std::string video_codec = "h264";
    std::string rc_mode = "cbr";
    std::string output = "-";
    std::string stream_profile;
    std::string input_format = "mjpeg";
    std::string video_range = "limited";
    std::string yuyv_converter = "libyuv";
    std::string rga_library;
    std::string listen_rtsp;
    std::string rtsp_path = "/capture";
    double audio_gain = 1.0;
    size_t audio_frame_frames = 960; // 20 ms at 48 kHz.
    bool video = true;
    bool audio = true;
    bool v4l2_dmabuf = false;
    bool mppjpeg_copy_output = false;
    bool rtsp_debug = false;
    bool quiet = false;
    int stats_interval_s = 5; // 0 disables periodic stats.
    int rtsp_idle_grace_ms = 3000;
    int rtsp_send_timeout_ms = 2000;
    int rtp_payload_size = 1400;
    int max_clients = 3;
    int width = 1920;
    int height = 1080;
    int fps = 50;
    int bitrate = 18000000;
    int gop = 50;
    int frames = 0; // 0 means run forever.
};

struct RtspProfileOptions {
    std::string name;
    Options opt;
};

struct Configuration {
    Options main;
    std::vector<RtspProfileOptions> profiles;
    bool check = false;
    bool print = false;
    bool show_version = false;
};

Configuration parse_configuration(int argc, char **argv);
void print_configuration(std::ostream &out, const Configuration &config);
std::string build_version_string();
