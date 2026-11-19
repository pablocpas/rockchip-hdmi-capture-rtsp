#include "options.hpp"
#include "rtsp_address.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <ostream>
#include <utility>

namespace {

[[noreturn]] void die(const std::string &message) {
    throw ConfigError(message);
}

int parse_int(const std::string &text) {
    const char *begin = text.data();
    const char *end = begin + text.size();
    if (begin != end && *begin == '+') ++begin;
    int value = 0;
    auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) die("invalid integer: " + text);
    return value;
}

double parse_double(const std::string &text) {
    char *end = nullptr;
    errno = 0;
    double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end || errno == ERANGE || !std::isfinite(value)) {
        die("invalid number: " + text);
    }
    return value;
}

int parse_bitrate(const std::string &text) {
    if (text.empty()) die("invalid bitrate: empty");
    const char suffix = text.back();
    double multiplier = 1.0;
    std::string num = text;
    if (suffix == 'k' || suffix == 'K') {
        multiplier = 1000.0;
        num.pop_back();
    } else if (suffix == 'm' || suffix == 'M') {
        multiplier = 1000000.0;
        num.pop_back();
    } else if (suffix == 'g' || suffix == 'G') {
        multiplier = 1000000000.0;
        num.pop_back();
    } else {
        return parse_int(text);
    }
    if (num.empty()) die("invalid bitrate: " + text);
    const double scaled = std::round(parse_double(num) * multiplier);
    if (!std::isfinite(scaled) || scaled <= 0.0 ||
        scaled > static_cast<double>(std::numeric_limits<int>::max())) {
        die("bitrate out of range: " + text);
    }
    return static_cast<int>(scaled);
}

std::pair<int, int> parse_size(const std::string &text) {
    const auto sep = text.find_first_of("xX");
    if (sep == std::string::npos || sep == 0 || sep + 1 >= text.size()) {
        die("invalid size (expected WxH, e.g. 1920x1080): " + text);
    }
    return {parse_int(text.substr(0, sep)), parse_int(text.substr(sep + 1))};
}

void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "General / Unix:\n"
            "  -c, --config PATH        Read INI config before CLI flags (- for stdin)\n"
            "      --check-config       Validate configuration and exit (0=ok, 2=invalid)\n"
            "      --print-config       Print effective normalized INI to stdout and exit\n"
            "  -q, --quiet              Suppress periodic stats logs on stderr\n"
            "  -v, --verbose            Enable verbose RTSP tracing (alias for --rtsp-debug)\n"
            "      --stats-interval N   Stats print interval in seconds, 0=off (default 5)\n"
            "  -V, --version            Print version and compiled features and exit\n"
            "  -h, --help               Show this help and exit\n"
            "\n"
            "Capture Input:\n"
            "  -d, --device PATH        V4L2 video device\n"
            "      --stream-profile P   Capture pipeline preset: mjpeg, rga, or yuyv-libyuv\n"
            "      --input-format FMT   mjpeg or yuyv/yuy2 (default mjpeg)\n"
            "      --yuyv-converter C   libyuv or rga for YUYV input (default libyuv)\n"
            "      --rga-library PATH   Override librga.so path for RGA convert/scale\n"
            "      --[no-]v4l2-dmabuf   Capture V4L2 directly into MPP/DRM DMA-BUF\n"
            "      --[no-]mppjpeg-copy-output  Copy MJPEG decoder output to encoder buffer\n"
            "  -s, --size WxH           Capture/stream resolution (e.g. 1920x1080, 1280x720)\n"
            "      --width N / --height N  Resolution width and height (default 1920x1080)\n"
            "  -r, --fps N              Capture/stream FPS (default 50)\n"
            "      --video-range R      full, limited, or unspecified (default limited)\n"
            "\n"
            "Video Encoder (MPP):\n"
            "      --[no-]video         Enable/disable video track (use --no-video for audio-only)\n"
            "      --video-codec C      h264 or h265/hevc (default h264)\n"
            "      --rc-mode MODE       cbr or vbr (default cbr)\n"
            "  -b, --bitrate RATE       Target bitrate, accepts k/M suffixes (e.g. 15M, 2.5M, 2500k)\n"
            "      --gop N              GOP length in frames (default 50)\n"
            "\n"
            "Audio Input (ALSA):\n"
            "      --[no-]audio         Enable/disable audio track (default enabled)\n"
            "      --audio-device ID    ALSA capture device (default hw:CARD=Video,DEV=0)\n"
            "      --audio-codec C      l16 or opus (default l16)\n"
            "      --audio-gain N       PCM gain multiplier before encoding (default 1.0)\n"
            "      --audio-frame-ms N   Frame duration: 2.5, 5, 10, 20, 40, or 60 (default 20)\n"
            "\n"
            "Output & Multi-Stream RTSP:\n"
            "  -o, --output PATH        Raw Annex-B file, '-' for stdout, or rtsp:// publish URL\n"
            "  -l, --listen-rtsp ADDR   Listen as RTSP/TCP server (e.g. :8554, [::1]:8554)\n"
            "      --rtsp-path PATH     Default RTSP stream path (default /capture)\n"
            "      --rtsp-stream SPEC   Add simultaneous on-demand stream (comma-separated k=v):\n"
            "                           path=/coche,size=1280x720,fps=25,codec=h265,bitrate=2M,rc-mode=vbr,send-timeout-ms=10000\n"
            "                           path=/audio,video=false,audio=true,send-timeout-ms=12000\n"
            "      --rtsp-profile SPEC  Alias for --rtsp-stream\n"
            "      --rtp-payload N      Max video RTP payload bytes (default 1400)\n"
            "      --max-clients N      Maximum concurrent RTSP clients (default 3)\n"
            "      --rtsp-idle-grace-ms N   Keep capture warm after last client (default 3000)\n"
            "      --rtsp-send-timeout-ms N Max stalled TCP send ms before drop (default 2000)\n"
            "      --[no-]rtsp-debug    Log RTSP session state transitions\n"
            "      --frames N           Stop after N encoded frames (0 = forever)\n"
            "\n"
            "Signals:\n"
            "  SIGINT, SIGTERM          Graceful shutdown\n"
            "  SIGUSR1                  Print live stats immediately to stderr\n"
            "  SIGUSR2                  Force immediate IDR keyframe on all active video streams\n"
            "\n"
            "All long options accept both '--opt value' and '--opt=value'.\n",
            argv0);
}

bool parse_bool_value(const std::string &value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    die("boolean value must be true or false: " + value);
}

void apply_stream_profile_defaults(Options &opt,
                                   const std::string &profile_name,
                                   bool input_format_set,
                                   bool yuyv_converter_set,
                                   bool v4l2_dmabuf_set) {
    opt.stream_profile = profile_name;
    if (opt.stream_profile == "default" || opt.stream_profile == "recommended") {
        opt.stream_profile = "mjpeg";
    }

    if (opt.stream_profile == "mjpeg") {
        if (!input_format_set) opt.input_format = "mjpeg";
        if (!yuyv_converter_set) opt.yuyv_converter = "libyuv";
        if (!v4l2_dmabuf_set) opt.v4l2_dmabuf = true;
    } else if (opt.stream_profile == "rga") {
        if (!input_format_set) opt.input_format = "yuyv";
        if (!yuyv_converter_set) opt.yuyv_converter = "rga";
        if (!v4l2_dmabuf_set) opt.v4l2_dmabuf = true;
    } else if (opt.stream_profile == "yuyv-libyuv" || opt.stream_profile == "libyuv") {
        opt.stream_profile = "yuyv-libyuv";
        if (!input_format_set) opt.input_format = "yuyv";
        if (!yuyv_converter_set) opt.yuyv_converter = "libyuv";
        if (!v4l2_dmabuf_set) opt.v4l2_dmabuf = false;
    } else {
        die("stream-profile must be mjpeg, rga, or yuyv-libyuv");
    }
}

size_t parse_audio_frame_ms(const char *value) {
    std::string s(value);
    if (s == "2.5") return 120;
    if (s == "5") return 240;
    if (s == "10") return 480;
    if (s == "20") return 960;
    if (s == "40") return 1920;
    if (s == "60") return 2880;
    die("--audio-frame-ms must be one of: 2.5, 5, 10, 20, 40, 60");
}

struct OptionSetFlags {
    bool input_format = false;
    bool yuyv_converter = false;
    bool v4l2_dmabuf = false;
};

std::string trim_copy(const std::string &s) {
    size_t begin = 0;
    while (begin < s.size() && isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string normalized_key(std::string key) {
    key = trim_copy(key);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c == '_' ? '-' : c));
    });
    return key;
}

std::string parse_config_value(std::string text, bool allow_comment = true) {
    text = trim_copy(text);
    if (text.empty()) return text;
    if (text.front() != '"' && text.front() != 39) {
        if (allow_comment) text.resize(std::min(text.size(), text.find_first_of("#;")));
        return trim_copy(text);
    }

    const char quote = text.front();
    std::string result;
    for (size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == quote) {
            const auto tail = trim_copy(text.substr(i + 1));
            if (!tail.empty() && !(allow_comment && (tail[0] == '#' || tail[0] == ';'))) {
                die("unexpected text after quoted value");
            }
            return result;
        }
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == quote || text[i + 1] == '\\')) {
            result += text[++i];
        } else {
            result += c;
        }
    }
    die("unterminated quoted value");
}

std::string ini_value(const std::string &value) {
    if (value == trim_copy(value) && value.find_first_of("#;\"'") == std::string::npos) return value;
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

void apply_option(Options &opt,
                  OptionSetFlags &flags,
                  const std::string &raw_key,
                  const std::string &raw_value,
                  const std::string &context) {
    const std::string &key = raw_key;
    const std::string &value = raw_value;
    if (value.find_first_of("\r\n") != std::string::npos) die("option values must fit on one line");
    if (context == "profile" &&
        (key == "listen-rtsp" || key == "output" || key == "max-clients" ||
         key == "rtsp-debug" || key == "rtsp-idle-grace-ms" || key == "rtp-payload" ||
         key == "cpu-governor" || key == "frames" || key == "quiet" || key == "stats-interval")) {
        die("option is global: " + key);
    }
    if (key == "device") opt.device = value;
    else if (key == "video-codec" || key == "codec") opt.video_codec = value == "hevc" ? "h265" : value;
    else if (key == "rc-mode") opt.rc_mode = value;
    else if (key == "video") opt.video = parse_bool_value(value);
    else if (key == "audio-device") opt.audio_device = value;
    else if (key == "audio-codec") opt.audio_codec = value;
    else if (key == "audio-gain") opt.audio_gain = parse_double(value);
    else if (key == "audio-frame-ms") opt.audio_frame_frames = parse_audio_frame_ms(value.c_str());
    else if (key == "audio") opt.audio = parse_bool_value(value);
    else if (key == "rtp-payload") opt.rtp_payload_size = parse_int(value);
    else if (key == "stream-profile" || key == "profile") opt.stream_profile = value;
    else if (key == "video-range") opt.video_range = value;
    else if (key == "input-format") {
        opt.input_format = value == "yuy2" ? "yuyv" : value;
        flags.input_format = true;
    } else if (key == "yuyv-converter") {
        opt.yuyv_converter = value;
        flags.yuyv_converter = true;
    } else if (key == "rga-library") opt.rga_library = value;
    else if (key == "v4l2-dmabuf") {
        opt.v4l2_dmabuf = parse_bool_value(value);
        flags.v4l2_dmabuf = true;
    } else if (key == "mppjpeg-copy-output") opt.mppjpeg_copy_output = parse_bool_value(value);
    else if (key == "output") opt.output = value;
    else if (key == "listen-rtsp") opt.listen_rtsp = value;
    else if (key == "rtsp-path" || key == "path") opt.rtsp_path = value;
    else if (key == "rtsp-debug") opt.rtsp_debug = parse_bool_value(value);
    else if (key == "quiet") opt.quiet = parse_bool_value(value);
    else if (key == "stats-interval") opt.stats_interval_s = parse_int(value);
    else if (key == "rtsp-idle-grace-ms") opt.rtsp_idle_grace_ms = parse_int(value);
    else if (key == "rtsp-send-timeout-ms" || key == "send-timeout-ms") {
        opt.rtsp_send_timeout_ms = parse_int(value);
    } else if (key == "max-clients") opt.max_clients = parse_int(value);
    else if (key == "cpu-governor") { /* Accepted for existing deployments. */ }
    else if (key == "decoder") {
        if (value != "mppjpeg") die("unsupported decoder '" + value + "'. Use mppjpeg.");
    } else if (key == "size") {
        const auto [w, h] = parse_size(value);
        opt.width = w;
        opt.height = h;
    } else if (key == "width") opt.width = parse_int(value);
    else if (key == "height") opt.height = parse_int(value);
    else if (key == "fps") opt.fps = parse_int(value);
    else if (key == "bitrate") opt.bitrate = parse_bitrate(value);
    else if (key == "gop") opt.gop = parse_int(value);
    else if (key == "frames") opt.frames = parse_int(value);
    else die("unknown " + context + " option: " + raw_key);
}

struct Setting {
    std::string key;
    std::string value;
    std::string origin;
};

struct ProfileSettings {
    std::string name;
    std::vector<Setting> settings;
};

void apply_setting(Options &opt, OptionSetFlags &flags, const Setting &setting,
                   bool profile = false) {
    try {
        apply_option(opt, flags, setting.key, setting.value, profile ? "profile" : "configuration");
    } catch (const std::exception &e) {
        die(setting.origin + ": " + setting.key + ": " + e.what());
    }
}

void load_config(std::istream &file, const std::string &path, std::vector<Setting> &main,
                 std::vector<ProfileSettings> &profiles) {
    ProfileSettings *profile = nullptr;
    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        std::string origin = path + ":" + std::to_string(line_no);
        line = trim_copy(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;

        if (line.front() == '[') {
            line.resize(std::min(line.size(), line.find_first_of("#;")));
            line = trim_copy(line);
            if (line.back() != ']') die(origin + ": expected [section]");
            std::string section = normalized_key(line.substr(1, line.size() - 2));
            if (section == "main" || section == "capture") {
                profile = nullptr;
            } else if (section.rfind("stream.", 0) == 0 && section.size() > 7) {
                profiles.push_back({section.substr(7), {}});
                profile = &profiles.back();
            } else if (section.rfind("profile.", 0) == 0 && section.size() > 8) {
                profiles.push_back({section.substr(8), {}});
                profile = &profiles.back();
            } else {
                die(origin + ": unknown section [" + section + "]");
            }
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) die(origin + ": expected key=value");
        try {
            Setting setting{normalized_key(line.substr(0, eq)), parse_config_value(line.substr(eq + 1)), origin};
            (profile ? profile->settings : main).push_back(std::move(setting));
        } catch (const std::exception &e) {
            die(origin + ": " + e.what());
        }
    }
    if (file.bad() || !file.eof()) die("failed reading configuration: " + path);
}

ProfileSettings parse_profile_spec(const std::string &spec) {
    ProfileSettings profile;
    size_t start = 0;
    while (start <= spec.size()) {
        const auto end = spec.find(',', start);
        std::string part = trim_copy(spec.substr(start, end - start));
        if (!part.empty()) {
            const auto eq = part.find('=');
            if (eq == std::string::npos) die("--rtsp-stream entries must be key=value: " + part);
            profile.settings.push_back({
                normalized_key(part.substr(0, eq)), parse_config_value(part.substr(eq + 1), false), "--rtsp-stream"
            });
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return profile;
}

void validate_options(Options &opt) {
    if (!opt.video && !opt.audio) {
        die("stream must enable video or audio");
    }
    if (opt.width <= 0 || opt.width > 8192 || opt.height <= 0 || opt.height > 8192 ||
        opt.width % 2 || opt.height % 2) {
        die("width and height must be even and between 2 and 8192");
    }
    if (opt.fps <= 0 || opt.fps > 240 || opt.bitrate <= 0 || opt.bitrate > 500000000 ||
        opt.gop <= 0 || opt.frames < 0) {
        die("fps must be 1–240, bitrate 1–500000000, gop positive and frames nonnegative");
    }
    if (opt.stats_interval_s < 0 || opt.stats_interval_s > 3600) {
        die("--stats-interval must be between 0 and 3600");
    }
    if (opt.audio_gain <= 0.0 || opt.audio_gain > 16.0) {
        die("--audio-gain must be > 0 and <= 16");
    }
    if (opt.rtp_payload_size < 256 || opt.rtp_payload_size > 60000) {
        die("--rtp-payload must be between 256 and 60000");
    }
    if (opt.max_clients <= 0 || opt.max_clients > 16) {
        die("--max-clients must be between 1 and 16");
    }
    if (opt.rtsp_idle_grace_ms < 0 || opt.rtsp_idle_grace_ms > 60000) {
        die("--rtsp-idle-grace-ms must be between 0 and 60000");
    }
    if (opt.rtsp_send_timeout_ms < 500 || opt.rtsp_send_timeout_ms > 60000) {
        die("--rtsp-send-timeout-ms must be between 500 and 60000");
    }
    if (opt.rtsp_path.empty()) die("RTSP path must not be empty");
    if (opt.rtsp_path[0] != '/') {
        opt.rtsp_path.insert(opt.rtsp_path.begin(), '/');
    }
    while (opt.rtsp_path.size() > 1 && opt.rtsp_path.back() == '/') opt.rtsp_path.pop_back();
    if (opt.rtsp_path.find_first_of("?# \t") != std::string::npos ||
        opt.rtsp_path.find("/trackID=") != std::string::npos) {
        die("invalid RTSP path: " + opt.rtsp_path);
    }
    if (opt.input_format == "yuy2") opt.input_format = "yuyv";
    if (opt.input_format != "mjpeg" && opt.input_format != "yuyv") {
        die("--input-format must be mjpeg or yuyv");
    }
    if (opt.yuyv_converter != "libyuv" && opt.yuyv_converter != "rga") {
        die("--yuyv-converter must be libyuv or rga");
    }
    if (opt.yuyv_converter == "rga" && opt.input_format != "yuyv") {
        die("--yuyv-converter rga requires --input-format yuyv");
    }
#ifndef HAVE_RGA
    if (opt.yuyv_converter == "rga") {
        die("RGA YUYV converter requested but this binary was built without librga");
    }
#endif
    if (opt.input_format == "yuyv" && opt.yuyv_converter == "rga") {
        opt.v4l2_dmabuf = true;
    } else if (opt.input_format != "mjpeg" && opt.v4l2_dmabuf) {
        opt.v4l2_dmabuf = false;
    }
    if (opt.video_codec == "hevc") opt.video_codec = "h265";
    if (opt.video_codec != "h264" && opt.video_codec != "h265") {
        die("--video-codec must be h264 or h265");
    }
    if (opt.rc_mode != "cbr" && opt.rc_mode != "vbr") {
        die("--rc-mode must be cbr or vbr");
    }
    if (opt.video_range != "full" && opt.video_range != "limited" && opt.video_range != "unspecified") {
        die("--video-range must be full, limited, or unspecified");
    }
    if (opt.audio_codec != "l16" && opt.audio_codec != "opus") {
        die("--audio-codec must be l16 or opus");
    }
#ifndef HAVE_OPUS
    if (opt.audio && opt.audio_codec == "opus") {
        die("opus audio requested but this binary was built without libopus");
    }
#endif
}

void apply_preset(Options &opt, const OptionSetFlags &flags) {
    if (!opt.stream_profile.empty()) {
        apply_stream_profile_defaults(opt, opt.stream_profile,
                                      flags.input_format, flags.yuyv_converter, flags.v4l2_dmabuf);
    }
}

RtspProfileOptions make_profile(const Options &base, const ProfileSettings &settings) {
    RtspProfileOptions profile{settings.name, base};
    profile.opt.rtsp_path.clear();
    profile.opt.frames = 0;
    OptionSetFlags flags;
    bool has_preset = false;
    for (const auto &setting : settings.settings) {
        if (setting.key == "name") {
            if (setting.value.find_first_of("\r\n") != std::string::npos) die("profile name must fit on one line");
            profile.name = setting.value;
        } else {
            apply_setting(profile.opt, flags, setting, true);
            has_preset |= setting.key == "stream-profile" || setting.key == "profile";
        }
    }
    if (has_preset) apply_preset(profile.opt, flags);
    try {
        validate_options(profile.opt);
        if (profile.opt.video) {
            if (profile.opt.width > base.width || profile.opt.height > base.height) {
                die("stream resolution cannot exceed capture resolution");
            }
            if (profile.opt.fps > base.fps) {
                die("stream fps cannot exceed capture fps");
            }
        }
    } catch (const std::exception &e) {
        die("stream " + (profile.name.empty() ? profile.opt.rtsp_path : profile.name) + ": " + e.what());
    }
    if (profile.name.empty()) {
        profile.name = profile.opt.rtsp_path == "/" ? "root" : profile.opt.rtsp_path.substr(1);
    }
    return profile;
}

std::string frame_duration(size_t frames) {
    return frames == 120 ? "2.5" : std::to_string(frames / 48);
}

void print_options(std::ostream &out, const Options &opt) {
    out << std::boolalpha << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "device=" << ini_value(opt.device) << '\n'
        << "audio-device=" << ini_value(opt.audio_device) << '\n'
        << "video=" << opt.video << '\n'
        << "audio=" << opt.audio << '\n'
        << "audio-codec=" << opt.audio_codec << '\n'
        << "audio-gain=" << opt.audio_gain << '\n'
        << "audio-frame-ms=" << frame_duration(opt.audio_frame_frames) << '\n'
        << "video-codec=" << opt.video_codec << '\n'
        << "rc-mode=" << opt.rc_mode << '\n'
        << "video-range=" << opt.video_range << '\n'
        << "stream-profile=" << opt.stream_profile << '\n'
        << "input-format=" << opt.input_format << '\n'
        << "yuyv-converter=" << opt.yuyv_converter << '\n'
        << "decoder=mppjpeg\n"
        << "v4l2-dmabuf=" << opt.v4l2_dmabuf << '\n'
        << "mppjpeg-copy-output=" << opt.mppjpeg_copy_output << '\n'
        << "rga-library=" << ini_value(opt.rga_library) << '\n'
        << "width=" << opt.width << '\n'
        << "height=" << opt.height << '\n'
        << "fps=" << opt.fps << '\n'
        << "bitrate=" << opt.bitrate << '\n'
        << "gop=" << opt.gop << '\n'
        << "rtsp-path=" << ini_value(opt.rtsp_path) << '\n'
        << "rtsp-send-timeout-ms=" << opt.rtsp_send_timeout_ms << '\n';
}

} // namespace

std::string build_version_string() {
    std::string s = "rk-hdmi-streamer 0.3.0 (";
#ifdef HAVE_RGA
    s += "rga=yes ";
#else
    s += "rga=no ";
#endif
#ifdef HAVE_OPUS
    s += "opus=yes ";
#else
    s += "opus=no ";
#endif
#ifdef HAVE_LIBYUV
    s += "libyuv=yes ";
#else
    s += "libyuv=no ";
#endif
#ifdef __ARM_NEON
    s += "neon=yes)";
#else
    s += "neon=no)";
#endif
    return s;
}

Configuration parse_configuration(int argc, char **argv) {
    Configuration config;
    std::vector<Setting> file_settings;
    std::vector<Setting> cli_settings;
    std::vector<ProfileSettings> file_profiles;
    std::vector<ProfileSettings> cli_profiles;

    for (int i = 1; i < argc; ++i) {
        const std::string raw = argv[i];
        std::string arg = raw;
        std::string inline_val;
        bool has_inline = false;
        if (raw.rfind("--", 0) == 0) {
            const auto eq = raw.find('=');
            if (eq != std::string::npos) {
                arg = raw.substr(0, eq);
                inline_val = raw.substr(eq + 1);
                has_inline = true;
            }
        }

        auto need_value = [&]() -> std::string {
            if (has_inline) return inline_val;
            if (i + 1 == argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
                die("missing value for " + arg);
            }
            return argv[++i];
        };

        auto reject_inline = [&]() {
            if (has_inline) die("flag does not take a value: " + arg);
        };

        if (arg == "--help" || arg == "-h") {
            reject_inline();
            usage(argv[0]);
            std::exit(0);
        } else if (arg == "--version" || arg == "-V") {
            reject_inline();
            config.show_version = true;
            return config;
        } else if (arg == "--config" || arg == "-c") {
            const auto path = need_value();
            if (path == "-") {
                load_config(std::cin, "stdin", file_settings, file_profiles);
            } else {
                std::ifstream file(path);
                if (!file) die("failed to open config file: " + path);
                load_config(file, path, file_settings, file_profiles);
            }
        } else if (arg == "--rtsp-profile" || arg == "--rtsp-stream") {
            cli_profiles.push_back(parse_profile_spec(need_value()));
        } else if (arg == "--check-config") {
            reject_inline();
            config.check = true;
        } else if (arg == "--print-config") {
            reject_inline();
            config.print = true;
        } else if (arg == "-d") {
            cli_settings.push_back({"device", need_value(), arg});
        } else if (arg == "-s") {
            cli_settings.push_back({"size", need_value(), arg});
        } else if (arg == "-r") {
            cli_settings.push_back({"fps", need_value(), arg});
        } else if (arg == "-b") {
            cli_settings.push_back({"bitrate", need_value(), arg});
        } else if (arg == "-o") {
            cli_settings.push_back({"output", need_value(), arg});
        } else if (arg == "-l") {
            cli_settings.push_back({"listen-rtsp", need_value(), arg});
        } else if (arg == "-q" || arg == "--quiet") {
            cli_settings.push_back({"quiet", has_inline ? inline_val : "true", arg});
        } else if (arg == "-v" || arg == "--verbose") {
            cli_settings.push_back({"rtsp-debug", has_inline ? inline_val : "true", arg});
        } else if (arg == "--video" || arg == "--no-video") {
            if (arg == "--no-video") reject_inline();
            cli_settings.push_back({"video", has_inline ? inline_val : (arg == "--video" ? "true" : "false"), arg});
        } else if (arg == "--audio" || arg == "--no-audio") {
            if (arg == "--no-audio") reject_inline();
            cli_settings.push_back({"audio", has_inline ? inline_val : (arg == "--audio" ? "true" : "false"), arg});
        } else if (arg == "--v4l2-dmabuf" || arg == "--no-v4l2-dmabuf") {
            if (arg == "--no-v4l2-dmabuf") reject_inline();
            cli_settings.push_back({"v4l2-dmabuf", has_inline ? inline_val : (arg == "--v4l2-dmabuf" ? "true" : "false"), arg});
        } else if (arg == "--mppjpeg-copy-output" || arg == "--no-mppjpeg-copy-output" ||
                   arg == "--no-mppjpeg-zero-copy") {
            const bool val = (arg != "--no-mppjpeg-copy-output");
            if (!val) reject_inline();
            cli_settings.push_back({"mppjpeg-copy-output", has_inline ? inline_val : (val ? "true" : "false"), arg});
        } else if (arg == "--rtsp-debug" || arg == "--no-rtsp-debug") {
            if (arg == "--no-rtsp-debug") reject_inline();
            cli_settings.push_back({"rtsp-debug", has_inline ? inline_val : (arg == "--rtsp-debug" ? "true" : "false"), arg});
        } else if (arg.rfind("--", 0) == 0) {
            cli_settings.push_back({normalized_key(arg.substr(2)), need_value(), arg});
        } else {
            die("unknown argument: " + raw);
        }
    }

    OptionSetFlags flags;
    for (const auto &setting : file_settings) apply_setting(config.main, flags, setting);
    for (const auto &setting : cli_settings) apply_setting(config.main, flags, setting);
    apply_preset(config.main, flags);
    validate_options(config.main);
    if (!config.main.listen_rtsp.empty()) {
        if (!rtsp::listen_address(config.main.listen_rtsp)) die("invalid RTSP listen address");
    } else if (config.main.output.rfind("rtsp://", 0) == 0) {
        if (!rtsp::publish_address(config.main.output)) die("invalid RTSP publish URL");
        if (config.main.video_codec != "h264") die("RTSP publisher mode requires H.264");
    }

    const std::string default_name = config.main.rtsp_path == "/" ? "root" : config.main.rtsp_path.substr(1);
    config.profiles.push_back({default_name, config.main});
    config.profiles.front().opt.frames = 0;
    auto add_profiles = [&](const std::vector<ProfileSettings> &specs) {
        for (const auto &spec : specs) {
            RtspProfileOptions profile = make_profile(config.main, spec);
            for (const auto &existing : config.profiles) {
                if (existing.opt.rtsp_path == profile.opt.rtsp_path) {
                    die("duplicate RTSP stream path: " + profile.opt.rtsp_path);
                }
            }
            config.profiles.push_back(std::move(profile));
        }
    };
    add_profiles(file_profiles);
    add_profiles(cli_profiles);
    return config;
}

void print_configuration(std::ostream &out, const Configuration &config) {
    out << "[capture]\n";
    const Options &opt = config.main;
    print_options(out, opt);
    out << "output=" << ini_value(opt.output) << '\n'
        << "listen-rtsp=" << ini_value(opt.listen_rtsp) << '\n'
        << "rtp-payload=" << opt.rtp_payload_size << '\n'
        << "rtsp-debug=" << opt.rtsp_debug << '\n'
        << "quiet=" << opt.quiet << '\n'
        << "stats-interval=" << opt.stats_interval_s << '\n'
        << "rtsp-idle-grace-ms=" << opt.rtsp_idle_grace_ms << '\n'
        << "max-clients=" << opt.max_clients << '\n'
        << "frames=" << opt.frames << '\n';
    for (size_t i = 1; i < config.profiles.size(); ++i) {
        const auto &profile = config.profiles[i];
        const bool section_safe = profile.name.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
        out << "\n[stream." << (section_safe ? profile.name : std::to_string(i)) << "]\n"
            << "name=" << ini_value(profile.name) << '\n';
        print_options(out, profile.opt);
    }
}
