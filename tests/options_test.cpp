#include "options.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

Configuration parse(std::initializer_list<std::string> options) {
    std::vector<std::string> args{"options-test"};
    args.insert(args.end(), options);
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    return parse_configuration(static_cast<int>(argv.size()), argv.data());
}

void rejects(std::initializer_list<std::string> args) {
    try {
        parse(args);
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error("invalid configuration accepted");
}

class ConfigFile {
public:
    explicit ConfigFile(const std::string &contents) {
        char filename[] = "/tmp/rk-options-test.XXXXXX";
        const int fd = mkstemp(filename);
        if (fd < 0) throw std::runtime_error("mkstemp failed");
        close(fd);
        path = filename;
        std::ofstream file(path);
        file << contents;
        file.close();
        if (!file) {
            unlink(path.c_str());
            throw std::runtime_error("failed to write test configuration");
        }
    }

    ~ConfigFile() { unlink(path.c_str()); }

    std::string path;
};

void validates_entire_number() {
    rejects({"--width", "1280px"});
    rejects({"--frames", "-1"});
    rejects({"--audio-gain", "nan"});
    rejects({"--fps", "99999999999999999999"});
    rejects({"--width", "1279"});
}

void applies_profile_defaults_independent_of_order() {
    Configuration preset_first = parse({"--rtsp-profile", "path=/extra,stream-profile=mjpeg,v4l2-dmabuf=false"});
    Configuration override_first = parse({"--rtsp-profile", "path=/extra,v4l2-dmabuf=false,stream-profile=mjpeg"});
    require(!preset_first.profiles.at(1).opt.v4l2_dmabuf, "profile override ignored");
    require(!override_first.profiles.at(1).opt.v4l2_dmabuf, "profile preset overrode explicit value");
    rejects({"--rtsp-profile", "name=missing-path"});
    rejects({"--rtsp-profile", "path=/capture/"});
}

void validates_rtsp_endpoints() {
    rejects({"--listen-rtsp", "127.0.0.1:"});
    rejects({"--listen-rtsp", ":65536"});
    rejects({"--output", "rtsp://127.0.0.1:8554"});
    require(parse({"--listen-rtsp", "[::1]:8554"}).main.listen_rtsp == "[::1]:8554",
            "bracketed IPv6 listen address rejected");
    require(parse({"--output", "rtsp://[::1]/capture"}).main.output == "rtsp://[::1]/capture",
            "bracketed IPv6 publish address rejected");
}

void mjpeg_requires_the_mpp_decoder() {
    const auto legacy = parse({"--check-config", "--decoder", "mppjpeg", "--stream-profile", "mjpeg"});
    require(legacy.main.v4l2_dmabuf, "the legacy MPP option must retain DMA-BUF");
    rejects({"--check-config", "--decoder", "turbojpeg"});
    ConfigFile file("[main]\n[profile.extra]\npath=/extra\ndecoder=turbojpeg\n");
    rejects({"--check-config", "--config", file.path});
}

void reads_and_prints_profiles() {
    ConfigFile file(
        "[main]\n"
        "stream-profile=mjpeg\n"
        "width=1280\n"
        "audio=false\n"
        "[profile.extra]\n"
        "path=/extra\n"
        "audio-device=hw:CARD=Other,DEV=0\n"
        "device=\"/tmp/device;with#comments\"\n"
        "codec=hevc\n");
    const auto config = parse({"--width", "1920", "--config", file.path, "--print-config"});
    const auto &profile = config.profiles.at(1).opt;
    require(config.print, "missing print action");
    require(profile.width == 1920 && !profile.audio, "profile did not inherit CLI and INI values");
    require(profile.audio_device == "hw:CARD=Other,DEV=0", "ALSA value was split at the comma");
    require(profile.device == "/tmp/device;with#comments", "quoted path was truncated");
    require(profile.video_codec == "h265", "codec alias not applied");

    std::ostringstream out;
    print_configuration(out, config);
    ConfigFile printed(out.str());
    auto roundtrip = parse({"--config", printed.path, "--check-config"});
    require(roundtrip.check, "missing check action");
    require(roundtrip.profiles.at(1).opt.device == profile.device, "printed path was truncated on reparse");
    require(roundtrip.profiles.at(1).opt.audio_device == profile.audio_device, "printed ALSA value changed");
    require(roundtrip.profiles.at(1).name == "extra", "profile name changed after round trip");
}

void parses_capture_and_simultaneous_streams() {
    ConfigFile file(
        "[capture]\n"
        "stream-profile=mjpeg\n"
        "width=1920\n"
        "height=1080\n"
        "fps=50\n"
        "rtsp-path=/hq\n"
        "rc-mode=vbr\n"
        "[stream.h265]\n"
        "path=/h265\n"
        "codec=h265\n"
        "bitrate=8000000\n"
        "[stream.coche]\n"
        "path=/coche\n"
        "codec=h265\n"
        "width=1280\n"
        "height=720\n"
        "fps=25\n"
        "bitrate=2500000\n"
        "rc-mode=vbr\n"
        "send-timeout-ms=8000\n"
        "[stream.audio]\n"
        "path=/audio\n"
        "video=false\n"
        "audio=true\n"
        "send-timeout-ms=8000\n");

    const auto config = parse({"--config", file.path, "--check-config"});
    require(config.profiles.size() == 4, "expected 4 configured RTSP streams");
    require(config.profiles[0].opt.rtsp_path == "/hq" && config.profiles[0].opt.rc_mode == "vbr",
            "capture default stream mismatch");
    require(config.profiles[1].opt.rtsp_path == "/h265" && config.profiles[1].opt.video_codec == "h265",
            "/h265 stream mismatch");
    const auto &coche = config.profiles[2].opt;
    require(coche.rtsp_path == "/coche" && coche.width == 1280 && coche.height == 720 &&
                coche.fps == 25 && coche.rc_mode == "vbr" && coche.rtsp_send_timeout_ms == 8000,
            "/coche stream mismatch");
    const auto &audio = config.profiles[3].opt;
    require(audio.rtsp_path == "/audio" && !audio.video && audio.audio && audio.rtsp_send_timeout_ms == 8000,
            "/audio stream mismatch");

    rejects({"--no-video", "--no-audio"});
    rejects({"--rc-mode", "abr"});
    rejects({"--rtsp-send-timeout-ms", "100"});
    rejects({"--width", "1280", "--rtsp-stream", "path=/big,width=1920"});
    rejects({"--fps", "25", "--rtsp-stream", "path=/fast,fps=50"});
}

void supports_unix_cli_ergonomics() {
    const auto cfg = parse({
        "-s", "1920x1080",
        "-r", "50",
        "-b", "15M",
        "--rc-mode=vbr",
        "-q",
        "--stats-interval=0",
        "-l", ":8554",
        "--rtsp-path=/hq",
        "--rtsp-stream=path=/coche,size=1280x720,fps=25,codec=h265,bitrate=2.5M,rc-mode=vbr,gop=25,send-timeout-ms=10000",
        "--rtsp-stream=path=/audio,video=false,audio=true,send-timeout-ms=12000",
    });
    require(cfg.main.width == 1920 && cfg.main.height == 1080 && cfg.main.fps == 50,
            "short -s/-r flags failed");
    require(cfg.main.bitrate == 15000000 && cfg.main.rc_mode == "vbr",
            "human bitrate suffix or --rc-mode=vbr failed");
    require(cfg.main.quiet && cfg.main.stats_interval_s == 0,
            "-q or --stats-interval=0 failed");
    require(cfg.profiles.size() == 3, "expected 3 streams");
    require(cfg.profiles[1].opt.width == 1280 && cfg.profiles[1].opt.height == 720 &&
                cfg.profiles[1].opt.bitrate == 2500000 && cfg.profiles[1].opt.rtsp_send_timeout_ms == 10000,
            "stream size/bitrate suffix parsing failed");
    require(!cfg.profiles[2].opt.video && cfg.profiles[2].opt.audio,
            "audio-only stream flag failed");

    const auto ver = parse({"-V"});
    require(ver.show_version, "-V must set show_version");

    rejects({"-s", "1920"});
    rejects({"-b", "M"});
    rejects({"--stats-interval=-1"});
}

} // namespace

int main() {
    try {
        validates_entire_number();
        applies_profile_defaults_independent_of_order();
        validates_rtsp_endpoints();
        mjpeg_requires_the_mpp_decoder();
        reads_and_prints_profiles();
        parses_capture_and_simultaneous_streams();
        supports_unix_cli_ergonomics();
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
