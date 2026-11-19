#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "audio_capture.hpp"
#include "options.hpp"
#include "rtsp_publisher.hpp"
#include "rtsp_server.hpp"
#include "stream_pipeline.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t stats_requested = 0;
volatile std::sig_atomic_t idr_requested = 0;

void handle_signal(int sig) {
    if (sig == SIGUSR1) {
        stats_requested = 1;
    } else if (sig == SIGUSR2) {
        idr_requested = 1;
    } else {
        stop_requested = 1;
    }
}

void install_signal_handlers() {
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, nullptr) < 0) {
        throw std::runtime_error("failed to ignore SIGPIPE");
    }

    struct sigaction action{};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) < 0 ||
        sigaction(SIGTERM, &action, nullptr) < 0 ||
        sigaction(SIGUSR1, &action, nullptr) < 0 ||
        sigaction(SIGUSR2, &action, nullptr) < 0) {
        throw std::runtime_error("failed to install signal handlers");
    }
}

class RawOutput {
public:
    explicit RawOutput(const std::string &path)
        : file_(path == "-" ? stdout : fopen(path.c_str(), "wb")) {
        if (!file_) throw std::runtime_error("failed to open output: " + path);
    }

    ~RawOutput() {
        if (file_ && file_ != stdout) fclose(file_);
    }

    void write(const uint8_t *data, size_t len) {
        if (len && fwrite(data, 1, len, file_) != len) throw std::runtime_error("output write failed");
    }

    void finish() {
        if (file_ == stdout) {
            if (fflush(file_) != 0) throw std::runtime_error("stdout flush failed");
        } else {
            if (fclose(std::exchange(file_, nullptr)) != 0) throw std::runtime_error("output close failed");
        }
    }

private:
    FILE *file_;
};

class CaptureClock {
public:
    uint32_t timestamp(uint64_t ns, int fps) {
        if (!started_) {
            started_ = true;
            base_ns_ = ns;
            return 0;
        }

        constexpr uint64_t second = 1000000000;
        const uint64_t delta = ns >= base_ns_ ? ns - base_ns_ : 0;
        uint64_t ticks = (delta / second) * 90000 + ((delta % second) * 90000 + second / 2) / second;
        if (ticks <= last_) ticks = last_ + std::max(1, 90000 / fps);
        last_ = ticks;
        return static_cast<uint32_t>(ticks);
    }

private:
    bool started_ = false;
    uint64_t base_ns_ = 0;
    uint64_t last_ = 0;
};

class StreamStats {
public:
    explicit StreamStats(int interval_s = 5, bool quiet = false)
        : interval_(std::chrono::seconds(interval_s)),
          enabled_(!quiet && interval_s > 0) {}

    CaptureStats capture;

    bool encoded() {
        ++encoded_;
        if (!enabled_) return false;
        const auto now = Clock::now();
        if (now - since_ < interval_) return false;
        flush(now);
        return true;
    }

    void flush(Clock::time_point now = Clock::now()) {
        const double seconds = std::max(1e-6, std::chrono::duration<double>(now - since_).count());
        fprintf(stderr, "captured=%llu encoded=%llu skipped=%llu avg_encoded_fps=%.2f\n",
                static_cast<unsigned long long>(capture.captured),
                static_cast<unsigned long long>(encoded_),
                static_cast<unsigned long long>(capture.skipped), encoded_ / seconds);
        capture = {};
        encoded_ = 0;
        since_ = now;
    }

private:
    std::chrono::seconds interval_{5};
    bool enabled_ = true;
    uint64_t encoded_ = 0;
    Clock::time_point since_ = Clock::now();
};

class RtspAudioOutput : public AudioOutput {
public:
    explicit RtspAudioOutput(RtspServer &server) : server_(server) {}

    void write_audio_l16_s16le(const int16_t *samples, size_t frames) override {
        server_.write_audio_l16_all(samples, frames);
    }

    void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) override {
        server_.write_audio_opus_all(payload, len, frames);
    }

private:
    RtspServer &server_;
};

std::vector<RtspServer::StreamProfile> server_profiles(const Configuration &config) {
    std::vector<RtspServer::StreamProfile> profiles;
    profiles.reserve(config.profiles.size());
    for (const auto &profile : config.profiles) {
        const Options &opt = profile.opt;
        std::function<std::vector<uint8_t>()> header_provider;
        if (opt.video) {
            header_provider = [opt] { return make_video_header(opt); };
        }
        profiles.push_back({
            profile.name,
            opt.rtsp_path,
            opt.video,
            opt.video_codec == "h265" ? VideoCodec::H265 : VideoCodec::H264,
            std::move(header_provider),
            opt.fps,
            opt.audio,
            opt.audio_codec,
            opt.audio_frame_frames,
            opt.rtsp_send_timeout_ms,
        });
        if (!config.main.quiet) {
            if (opt.video) {
                fprintf(stderr, "rtsp stream=%s path=%s %dx%d@%d codec=%s rc=%s bitrate=%d audio=%s timeout=%dms\n",
                        profile.name.c_str(), opt.rtsp_path.c_str(), opt.width, opt.height,
                        opt.fps, opt.video_codec.c_str(), opt.rc_mode.c_str(), opt.bitrate,
                        opt.audio ? opt.audio_codec.c_str() : "off", opt.rtsp_send_timeout_ms);
            } else {
                fprintf(stderr, "rtsp stream=%s path=%s audio-only codec=%s timeout=%dms\n",
                        profile.name.c_str(), opt.rtsp_path.c_str(), opt.audio_codec.c_str(),
                        opt.rtsp_send_timeout_ms);
            }
        }
    }
    return profiles;
}

void run_rtsp(const Configuration &config) {
    const Options &opts = config.main;
    RtspServer server(opts.listen_rtsp, server_profiles(config), opts.rtp_payload_size,
                      opts.rtsp_debug, opts.max_clients);
    RtspAudioOutput audio_output(server);
    AudioRuntime audio;

    struct StreamRuntime {
        RtspServer::StreamSelection selection{};
        CaptureClock clock{};
        Clock::time_point idle_since = Clock::time_point::min();
    };
    std::vector<StreamRuntime> stream_state(config.profiles.size());
    const auto idle_grace = std::chrono::milliseconds(opts.rtsp_idle_grace_ms);

    std::unique_ptr<StreamPipeline> pipeline;
    auto capture_idle_since = Clock::time_point::min();
    auto retry_at = Clock::time_point::min();
    auto retry_delay = 1s;
    auto last_video = Clock::time_point::min();
    StreamStats stats(opts.stats_interval_s, opts.quiet);
    uint64_t encoded = 0;

    const StreamPipeline::MultiPacketWriter write = [&](int stream_id, EncodedPacket packet, uint64_t ns) {
        const auto idx = static_cast<size_t>(stream_id);
        auto &st = stream_state[idx];
        if (!st.selection) return;
        packet.rtp_timestamp = st.clock.timestamp(ns, config.profiles[idx].opt.fps);
        packet.has_rtp_timestamp = true;
        server.write_video(packet, st.selection);
    };

    while (!stop_requested && (opts.frames == 0 || encoded < static_cast<uint64_t>(opts.frames))) {
        if (stats_requested) {
            stats_requested = 0;
            stats.flush();
            server.log_stats();
        }
        if (idr_requested) {
            idr_requested = 0;
            server.request_keyframe();
        }

        const auto active = server.active_streams();
        const auto now = Clock::now();

        std::vector<RtspServer::StreamSelection> desired(config.profiles.size());
        bool any_audio = false;
        bool any_video = false;
        for (const auto &sel : active) {
            const auto idx = static_cast<size_t>(sel.profile_index);
            desired[idx] = sel;
            if (config.profiles[idx].opt.audio) any_audio = true;
            if (config.profiles[idx].opt.video) any_video = true;
        }

        if (any_audio) {
            audio.start(config.main, audio_output);
        } else {
            audio.stop();
        }

        if (!any_video) {
            if (pipeline) {
                pipeline->stop();
                if (capture_idle_since == Clock::time_point::min()) capture_idle_since = now;
                if (now - capture_idle_since >= idle_grace) {
                    pipeline.reset();
                    for (auto &st : stream_state) st = {};
                }
            }
            if (any_audio) {
                std::this_thread::sleep_for(100ms);
            } else {
                server.wait_for_clients(pipeline ? 100ms : 500ms);
            }
            continue;
        }

        capture_idle_since = Clock::time_point::min();
        if (now < retry_at) {
            std::this_thread::sleep_for(100ms);
            continue;
        }

        try {
            if (!pipeline) {
                pipeline = std::make_unique<StreamPipeline>(config.main, false);
                pipeline->start();
                last_video = Clock::now();
                if (!opts.quiet) {
                    fprintf(stderr, "capture ready device=%s %dx%d@%d\n",
                            config.main.device.c_str(), config.main.width, config.main.height, config.main.fps);
                }
            } else {
                pipeline->start();
            }

            for (size_t i = 0; i < config.profiles.size(); ++i) {
                if (!config.profiles[i].opt.video) continue;
                auto &st = stream_state[i];
                const int sid = static_cast<int>(i);
                if (desired[i]) {
                    st.idle_since = Clock::time_point::min();
                    const bool changed = (st.selection != desired[i]);
                    const bool added = !pipeline->has_stream(sid);
                    if (added) {
                        const auto &hdr = pipeline->enable_stream(sid, config.profiles[i].opt);
                        server.set_video_header(desired[i], hdr);
                    }
                    if (changed || added) {
                        st.selection = desired[i];
                        st.clock = {};
                        server.request_keyframe(sid);
                    }
                    if (server.consume_keyframe_request(sid)) {
                        pipeline->request_idr(sid);
                    }
                } else {
                    st.selection = {};
                    if (pipeline->has_stream(sid)) {
                        if (st.idle_since == Clock::time_point::min()) st.idle_since = now;
                        if (now - st.idle_since >= idle_grace) {
                            pipeline->disable_stream(sid);
                            st.idle_since = Clock::time_point::min();
                        }
                    }
                }
            }

            if (pipeline->read_frame_multi(write, stats.capture)) {
                last_video = Clock::now();
                retry_delay = 1s;
                ++encoded;
                if (stats.encoded()) server.log_stats();
            } else if (Clock::now() - last_video >= 3s) {
                throw std::runtime_error("no usable video for 3 seconds");
            }
        } catch (const std::exception &e) {
            if (stop_requested) break;
            fprintf(stderr, "capture error: %s; retrying in %lld s\n",
                    e.what(), static_cast<long long>(retry_delay.count()));
            for (const auto &sel : active) {
                if (config.profiles[static_cast<size_t>(sel.profile_index)].opt.video) {
                    server.invalidate_video(sel);
                }
            }
            pipeline.reset();
            for (auto &st : stream_state) st = {};
            retry_at = Clock::now() + retry_delay;
            retry_delay = std::min(5s, retry_delay * 2);
        }
    }
}

void run_output(const Options &opt) {
    StreamPipeline pipeline(opt);
    const auto &header = pipeline.video_header();
    std::unique_ptr<MediaOutput> publisher;
    std::unique_ptr<RawOutput> raw;
    AudioRuntime audio;
    if (opt.output.rfind("rtsp://", 0) == 0) {
        publisher = make_rtsp_publisher(opt, header);
        audio.start(opt, *publisher);
    } else {
        raw = std::make_unique<RawOutput>(opt.output);
        raw->write(header.data(), header.size());
    }
    pipeline.start();

    CaptureClock clock;
    const StreamPipeline::PacketWriter write = [&](EncodedPacket packet, uint64_t ns) {
        if (publisher) {
            packet.rtp_timestamp = clock.timestamp(ns, opt.fps);
            packet.has_rtp_timestamp = true;
            publisher->write_packet(packet);
        } else {
            raw->write(packet.data, packet.len);
        }
    };
    StreamStats stats(opt.stats_interval_s, opt.quiet);
    uint64_t encoded = 0;
    while (!stop_requested && (opt.frames == 0 || encoded < static_cast<uint64_t>(opt.frames))) {
        if (stats_requested) {
            stats_requested = 0;
            stats.flush();
            if (publisher) publisher->log_stats();
        }
        if (idr_requested) {
            idr_requested = 0;
            pipeline.request_idr();
        }
        try {
            if (pipeline.read_frame(write, stats.capture)) {
                ++encoded;
                if (stats.encoded() && publisher) publisher->log_stats();
            }
        } catch (...) {
            if (stop_requested) break;
            throw;
        }
    }
    if (raw) raw->finish();
}

} // namespace

int main(int argc, char **argv) {
    try {
        const auto config = parse_configuration(argc, argv);
        if (config.show_version) {
            std::cout << build_version_string() << '\n';
            return std::cout ? 0 : 1;
        }
        if (config.print) {
            print_configuration(std::cout, config);
            std::cout.flush();
            return std::cout ? 0 : 1;
        }
        if (config.check) return 0;

        install_signal_handlers();
        const Options &opt = config.main;
        if (!opt.quiet) {
            fprintf(stderr, "capture=%s %dx%d@%d format=%s codec=%s rc=%s bitrate=%d\n",
                    opt.device.c_str(), opt.width, opt.height, opt.fps, opt.input_format.c_str(),
                    opt.video_codec.c_str(), opt.rc_mode.c_str(), opt.bitrate);
        }
        if (opt.listen_rtsp.empty()) run_output(opt);
        else run_rtsp(config);
        return 0;
    } catch (const ConfigError &e) {
        fprintf(stderr, "config error: %s\n", e.what());
        return 2;
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
