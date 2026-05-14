#pragma once

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/tcp.h>

#include "h264_utils.hpp"
#include "media_types.hpp"

[[noreturn]] inline void rtsp_die(const std::string &msg) {
    throw std::runtime_error(msg);
}

class RtspServer : public MediaOutput {
public:
    struct StreamProfile {
        std::string name;
        std::string path;
        VideoCodec video_codec = VideoCodec::H264;
        std::function<std::vector<uint8_t>()> video_header_provider;
        int fps = 30;
        bool audio = false;
        std::string audio_codec;
        size_t audio_frame_frames = 960;
    };

private:
    struct ProfileState {
        StreamProfile profile;
        bool video_header_loaded = false;
        std::vector<uint8_t> sps;
        std::vector<uint8_t> pps;
        std::vector<uint8_t> vps;
    };

public:
    RtspServer(const std::string &listen_addr,
               std::vector<StreamProfile> profiles,
               int rtp_payload_size,
               bool rtsp_debug,
               int max_clients)
        : listen_addr_(listen_addr),
          max_payload_(static_cast<size_t>(rtp_payload_size)),
          rtsp_debug_(rtsp_debug),
          max_clients_(max_clients) {
        if (profiles.empty()) rtsp_die("RTSP server requires at least one profile");
        profiles_.reserve(profiles.size());
        for (auto &profile : profiles) {
            if (profile.path.empty() || profile.path[0] != '/') {
                rtsp_die("RTSP profile path must start with /");
            }
            if (!profile.video_header_provider) {
                rtsp_die("RTSP profile requires a video header provider");
            }
            ProfileState state;
            state.profile = std::move(profile);
            profiles_.push_back(std::move(state));
        }
        listen_tcp();
        running_.store(true, std::memory_order_relaxed);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    RtspServer(const std::string &listen_addr,
               const std::string &path,
               VideoCodec video_codec,
               const std::vector<uint8_t> &video_header,
               int fps,
               bool audio,
               const std::string &audio_codec,
               int rtp_payload_size,
               size_t audio_frame_frames,
               bool rtsp_debug,
               int max_clients)
        : RtspServer(listen_addr,
                     std::vector<StreamProfile>{StreamProfile{
                         "capture",
                         path,
                         video_codec,
                         [video_header] { return video_header; },
                         fps,
                         audio,
                         audio_codec,
                         audio_frame_frames,
                     }},
                     rtp_payload_size,
                     rtsp_debug,
                     max_clients) {}

    ~RtspServer() {
        running_.store(false, std::memory_order_relaxed);
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();

        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions = sessions_;
        }
        for (const auto &session : sessions) session->stop();
        for (const auto &session : sessions) session->join();
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.clear();
        }
    }

    bool has_clients() const {
        return active_readers_.load(std::memory_order_relaxed) > 0;
    }

    int active_profile_index() const {
        std::lock_guard<std::mutex> lock(active_mutex_);
        return active_profile_index_;
    }

    bool wait_for_clients(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(active_mutex_);
        active_cv_.wait_for(lock, timeout, [&] {
            return !running_.load(std::memory_order_relaxed) || has_clients();
        });
        return has_clients();
    }

    bool consume_keyframe_request() {
        return keyframe_requested_.exchange(false, std::memory_order_relaxed);
    }

    void write_frame(const uint8_t *data, size_t len) override {
        if (!has_clients()) return;

        EncodedPacket packet{data, len, {}};
        write_packet(packet);
    }

    void write_packet(const EncodedPacket &packet) override {
        if (!has_clients() || !packet.data || packet.len == 0) return;
        const ProfileState &profile = active_profile();
        uint32_t timestamp = packet.has_rtp_timestamp
            ? packet.rtp_timestamp
            : video_timestamp_.load(std::memory_order_relaxed);

        RtpStorage storage;
        storage.packet = packet.mpp_packet;
        if (storage.packet) {
            storage.data = packet.data;
            storage.size = packet.len;
        } else {
            storage.bytes = std::make_shared<std::vector<uint8_t>>(packet.data, packet.data + packet.len);
            storage.data = storage.bytes->data();
            storage.size = storage.bytes->size();
        }

        EncodedPacket storage_packet{storage.data, storage.size, storage.packet, timestamp, true};
        auto nals = video_nals(profile, storage_packet);
        if (nals.empty()) return;

        size_t last_payload_nal = nals.size();
        for (size_t i = nals.size(); i > 0; --i) {
            if (!is_video_aud(profile, nals[i - 1])) {
                last_payload_nal = i - 1;
                break;
            }
        }
        if (last_payload_nal == nals.size()) return;

        std::vector<RtpChunk> chunks;
        chunks.reserve(nals.size() + storage.size / max_payload_ + 1);
        for (size_t i = 0; i < nals.size(); ++i) {
            if (is_video_aud(profile, nals[i])) continue;
            size_t offset = static_cast<size_t>(nals[i].data - storage.data);
            add_video_nal(profile, chunks, storage, timestamp, offset, nals[i].size, i == last_payload_nal);
        }
        broadcast(chunks);
        if (packet.has_rtp_timestamp) {
            video_timestamp_.store(timestamp, std::memory_order_relaxed);
        } else {
            video_timestamp_.fetch_add(90000 / profile.profile.fps, std::memory_order_relaxed);
        }
    }

    void write_audio_l16_s16le(const int16_t *samples, size_t frames) override {
        if (!active_audio_enabled() || frames == 0 || !has_clients()) return;

        constexpr size_t channels = 2;
        auto payload = std::make_shared<std::vector<uint8_t>>(frames * channels * sizeof(int16_t));
        for (size_t i = 0; i < frames * channels; ++i) {
            uint16_t sample = static_cast<uint16_t>(samples[i]);
            (*payload)[i * 2 + 0] = static_cast<uint8_t>(sample >> 8);
            (*payload)[i * 2 + 1] = static_cast<uint8_t>(sample);
        }

        RtpChunk chunk{};
        chunk.channel = 2;
        chunk.payload_type = 97;
        chunk.timestamp = audio_timestamp_.fetch_add(static_cast<uint32_t>(frames), std::memory_order_relaxed);
        chunk.marker = audio_marker_.exchange(false, std::memory_order_relaxed);
        chunk.storage.bytes = payload;
        chunk.storage.data = payload->data();
        chunk.storage.size = payload->size();
        chunk.offset = 0;
        chunk.size = payload->size();
        broadcast(std::vector<RtpChunk>{chunk});
    }

    void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) override {
        if (!active_audio_enabled() || len == 0 || frames == 0 || !has_clients()) return;
        auto data = std::make_shared<std::vector<uint8_t>>(payload, payload + len);
        RtpChunk chunk{};
        chunk.channel = 2;
        chunk.payload_type = 97;
        chunk.timestamp = audio_timestamp_.fetch_add(static_cast<uint32_t>(frames), std::memory_order_relaxed);
        chunk.marker = audio_marker_.exchange(false, std::memory_order_relaxed);
        chunk.storage.bytes = data;
        chunk.storage.data = data->data();
        chunk.storage.size = data->size();
        chunk.offset = 0;
        chunk.size = data->size();
        broadcast(std::vector<RtpChunk>{chunk});
    }

    void log_stats() override {
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - stats_last_time_).count();
        if (secs <= 0.0) return;
        uint64_t bytes = send_bytes_.load(std::memory_order_relaxed);
        uint64_t packets = rtp_packets_.load(std::memory_order_relaxed);
        uint64_t drops = queue_drops_.load(std::memory_order_relaxed);
        uint64_t d_bytes = bytes - stats_last_send_bytes_;
        uint64_t d_packets = packets - stats_last_rtp_packets_;
        uint64_t d_drops = drops - stats_last_queue_drops_;
        fprintf(stderr,
                "rtsp server clients=%d net Mbps=%.2f rtp_packets/s=%.0f queue_drops=%llu\n",
                active_readers_.load(std::memory_order_relaxed),
                static_cast<double>(d_bytes) * 8.0 / secs / 1000000.0,
                static_cast<double>(d_packets) / secs,
                static_cast<unsigned long long>(d_drops));
        int profile_index = active_profile_index();
        bool active_h265 = profile_index >= 0 &&
            profile_index < static_cast<int>(profiles_.size()) &&
            profiles_[static_cast<size_t>(profile_index)].profile.video_codec == VideoCodec::H265;
        if (rtsp_debug_ && active_h265) {
            uint64_t h265_single_nals = h265_single_nals_.load(std::memory_order_relaxed);
            uint64_t h265_fu_nals = h265_fu_nals_.load(std::memory_order_relaxed);
            uint64_t h265_fu_packets = h265_fu_packets_.load(std::memory_order_relaxed);
            uint64_t h265_invalid_nals = h265_invalid_nals_.load(std::memory_order_relaxed);
            uint64_t d_single = h265_single_nals - stats_last_h265_single_nals_;
            uint64_t d_fu_nals = h265_fu_nals - stats_last_h265_fu_nals_;
            uint64_t d_fu_packets = h265_fu_packets - stats_last_h265_fu_packets_;
            uint64_t d_invalid = h265_invalid_nals - stats_last_h265_invalid_nals_;
            fprintf(stderr,
                    "rtsp h265 single_nals/s=%.0f fu_nals/s=%.0f fu_packets/s=%.0f invalid_nals/s=%.0f\n",
                    static_cast<double>(d_single) / secs,
                    static_cast<double>(d_fu_nals) / secs,
                    static_cast<double>(d_fu_packets) / secs,
                    static_cast<double>(d_invalid) / secs);
            stats_last_h265_single_nals_ = h265_single_nals;
            stats_last_h265_fu_nals_ = h265_fu_nals;
            stats_last_h265_fu_packets_ = h265_fu_packets;
            stats_last_h265_invalid_nals_ = h265_invalid_nals;
        }
        stats_last_time_ = now;
        stats_last_send_bytes_ = bytes;
        stats_last_rtp_packets_ = packets;
        stats_last_queue_drops_ = drops;
    }

private:
    struct RtpStorage {
        SharedMppPacket packet;
        std::shared_ptr<std::vector<uint8_t>> bytes;
        const uint8_t *data = nullptr;
        size_t size = 0;
    };

    struct RtpChunk {
        uint8_t channel = 0;
        uint8_t payload_type = 96;
        uint32_t timestamp = 0;
        bool marker = false;
        RtpStorage storage;
        size_t offset = 0;
        size_t size = 0;
        uint8_t prefix[3] = {};
        size_t prefix_size = 0;

        size_t payload_size() const { return prefix_size + size; }
        size_t wire_size() const { return 4 + 12 + payload_size(); }
    };

    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(RtspServer &server, int fd, uint64_t id)
            : server_(server), fd_(fd), id_(id) {
            int yes = 1;
            setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            cname_ = "rk-hdmi-streamer-" + std::to_string(id_);
            reset_track_for_new_epoch(video_track_, 0, 1);
            reset_track_for_new_epoch(audio_track_, 2, 3);
        }

        ~Session() {
            stop();
            join();
        }

        void stop() {
            closed_.store(true, std::memory_order_relaxed);
            close_socket();
            queue_cv_.notify_all();
        }

        void join() {
            join_thread(read_thread_);
            join_thread(write_thread_);
        }

        void start() {
            auto self = shared_from_this();
            read_thread_ = std::thread([self] { self->read_loop(); });
            write_thread_ = std::thread([self] { self->write_loop(); });
        }

        bool closed() const { return closed_.load(std::memory_order_relaxed); }
        bool playing() const { return playing_.load(std::memory_order_relaxed); }
        uint64_t id() const { return id_; }

        void enqueue(const std::vector<RtpChunk> &chunks) {
            if (!playing() || closed()) return;
            std::lock_guard<std::mutex> lock(queue_mutex_);
            std::vector<const RtpChunk *> accepted;
            accepted.reserve(chunks.size());
            size_t frame_bytes = 0;
            for (const auto &chunk : chunks) {
                if (chunk.channel == 0 && !video_track_.setup) continue;
                if (chunk.channel == 2 && !audio_track_.setup) continue;
                size_t bytes = chunk.wire_size();
                if (bytes > max_queue_bytes_) continue;
                accepted.push_back(&chunk);
                frame_bytes += bytes;
            }
            if (accepted.empty()) return;
            if (frame_bytes > max_queue_bytes_) {
                dropped_ += accepted.size();
                server_.queue_drops_.fetch_add(accepted.size(), std::memory_order_relaxed);
                return;
            }
            if (queue_bytes_ + frame_bytes > max_queue_bytes_) {
                size_t dropped = queue_.size();
                queue_.clear();
                queue_bytes_ = 0;
                dropped_ += dropped;
                server_.queue_drops_.fetch_add(dropped, std::memory_order_relaxed);
                trace("queue_clear", "client is not reading fast enough");
            }
            for (const auto *chunk : accepted) {
                queue_.push_back(*chunk);
                queue_bytes_ += chunk->wire_size();
            }
            queue_cv_.notify_one();
        }

    private:
        static constexpr size_t max_queue_bytes_ = 2 * 1024 * 1024;
        static constexpr auto rtcp_interval_ = std::chrono::seconds(5);
        static constexpr auto session_timeout_ = std::chrono::seconds(60);

        enum class State {
            Init,
            Ready,
            Playing,
            Teardown,
        };

        void join_thread(std::thread &thread) {
            if (!thread.joinable()) return;
            if (thread.get_id() == std::this_thread::get_id()) {
                thread.detach();
                return;
            }
            thread.join();
        }

        struct TrackState {
            bool setup = false;
            uint8_t rtp_channel = 0;
            uint8_t rtcp_channel = 1;
            uint16_t seq = 0;
            uint32_t ssrc = 0;
            uint32_t timestamp_base = 0;
            uint32_t last_rtp_timestamp = 0;
            uint32_t packet_count = 0;
            uint32_t octet_count = 0;
            uint32_t rtcp_rx_count = 0;
            std::string cname;
            std::chrono::steady_clock::time_point last_rtcp = std::chrono::steady_clock::time_point::min();
        };

        void reset_track_for_new_epoch(TrackState &track, uint8_t rtp_channel, uint8_t rtcp_channel) {
            track.rtp_channel = rtp_channel;
            track.rtcp_channel = rtcp_channel;
            track.seq = static_cast<uint16_t>(server_.random_u32());
            track.ssrc = server_.random_u32();
            track.timestamp_base = server_.random_u32();
            track.last_rtp_timestamp = track.timestamp_base;
            track.packet_count = 0;
            track.octet_count = 0;
            track.rtcp_rx_count = 0;
            track.cname = cname_;
            track.last_rtcp = std::chrono::steady_clock::time_point::min();
        }

        void reset_tracks_for_new_epoch() {
            reset_track_for_new_epoch(video_track_, video_track_.rtp_channel, video_track_.rtcp_channel);
            reset_track_for_new_epoch(audio_track_, audio_track_.rtp_channel, audio_track_.rtcp_channel);
        }

        void clear_queue() {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.clear();
            queue_bytes_ = 0;
        }

        void trace(const char *event, const std::string &detail = {}) {
            if (!server_.rtsp_debug_) return;
            fprintf(stderr, "rtsp session=%llu %s%s%s\n",
                    static_cast<unsigned long long>(id_),
                    event,
                    detail.empty() ? "" : " ",
                    detail.c_str());
        }

        void read_loop() {
            std::string pending;
            trace("accepted");
            while (!closed()) {
                std::string req;
                if (!read_request(pending, req)) break;
                if (!handle_request(req)) break;
            }
            bool was_playing = playing_.exchange(false, std::memory_order_relaxed);
            if (was_playing) server_.reader_stopped();
            if (!closed()) send_rtcp_bye_for_setup_tracks();
            clear_queue();
            state_ = State::Teardown;
            if (server_.rtsp_debug_) {
                std::ostringstream detail;
                detail << "video_rtp=" << video_track_.packet_count
                       << " audio_rtp=" << audio_track_.packet_count
                       << " video_rtcp_rx=" << video_track_.rtcp_rx_count
                       << " audio_rtcp_rx=" << audio_track_.rtcp_rx_count;
                trace("stats", detail.str());
            }
            closed_.store(true, std::memory_order_relaxed);
            close_socket();
            queue_cv_.notify_all();
            server_.remove_session(id_);
            trace("closed");
        }

        bool read_request(std::string &pending, std::string &req) {
            while (true) {
                bool waiting_for_interleaved_frame = false;
                while (!pending.empty() && pending[0] == '$') {
                    if (pending.size() < 4) {
                        waiting_for_interleaved_frame = true;
                        break;
                    }
                    size_t frame_len =
                        (static_cast<uint8_t>(pending[2]) << 8) |
                        static_cast<uint8_t>(pending[3]);
                    if (pending.size() < 4 + frame_len) {
                        waiting_for_interleaved_frame = true;
                        break;
                    }
                    handle_interleaved_frame(static_cast<uint8_t>(pending[1]),
                                             reinterpret_cast<const uint8_t *>(pending.data() + 4),
                                             frame_len);
                    last_activity_ = std::chrono::steady_clock::now();
                    pending.erase(0, 4 + frame_len);
                }

                if (!waiting_for_interleaved_frame) {
                    size_t hdr_end = pending.find("\r\n\r\n");
                    if (hdr_end != std::string::npos) {
                        size_t header_len = hdr_end + 4;
                        size_t body_len = content_length_from_header(pending.substr(0, header_len));
                        if (body_len > 65536) return false;
                        if (pending.size() < header_len + body_len) {
                            // Wait for the complete RTSP message body so it cannot pollute
                            // the next request on this persistent connection.
                        } else {
                            req = pending.substr(0, header_len + body_len);
                            pending.erase(0, header_len + body_len);
                            last_activity_ = std::chrono::steady_clock::now();
                            return true;
                        }
                    }
                }

                auto now = std::chrono::steady_clock::now();
                if (now - last_activity_ > session_timeout_) {
                    trace("timeout");
                    return false;
                }

                int fd = fd_.load(std::memory_order_relaxed);
                if (fd < 0) return false;
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;
                int pret = poll(&pfd, 1, 1000);
                if (pret == 0) continue;
                if (pret < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
                char buf[2048];
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) return false;
                last_activity_ = std::chrono::steady_clock::now();
                pending.append(buf, buf + n);
                if (pending.size() > 65536) return false;
            }
        }

        void handle_interleaved_frame(uint8_t channel, const uint8_t *data, size_t len) {
            if (len < 2) return;
            TrackState *track = nullptr;
            if (video_track_.setup && channel == video_track_.rtcp_channel) track = &video_track_;
            if (audio_track_.setup && channel == audio_track_.rtcp_channel) track = &audio_track_;
            if (!track) return;
            uint8_t rtcp_type = data[1];
            if (rtcp_type >= 192 && rtcp_type <= 223) ++track->rtcp_rx_count;
        }

        static std::string first_line(const std::string &req) {
            size_t end = req.find("\r\n");
            return req.substr(0, end);
        }

        static std::string lower(std::string s) {
            for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string header_value(const std::string &req, const std::string &name) {
            std::string key = "\r\n" + lower(name) + ":";
            std::string low = lower(req);
            size_t pos = low.find(key);
            if (pos == std::string::npos) return {};
            pos += key.size();
            while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) ++pos;
            size_t end = req.find("\r\n", pos);
            return req.substr(pos, end - pos);
        }

        static std::string session_token(std::string value) {
            size_t semi = value.find(';');
            if (semi != std::string::npos) value.resize(semi);
            size_t begin = 0;
            while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
            size_t end = value.size();
            while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) --end;
            return value.substr(begin, end - begin);
        }

        bool request_session_matches(const std::string &req) {
            if (session_id_.empty()) return false;
            return session_token(header_value(req, "Session")) == session_id_;
        }

        bool require_session(const std::string &req, const std::string &cseq) {
            if (request_session_matches(req)) return true;
            send_error(cseq, 454, "Session Not Found");
            return false;
        }

        static size_t content_length_from_header(const std::string &header) {
            std::string low = lower(header);
            std::string key = "\r\ncontent-length:";
            size_t pos = low.find(key);
            if (pos == std::string::npos) return 0;
            pos += key.size();
            while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) ++pos;
            char *end = nullptr;
            unsigned long len = strtoul(header.c_str() + pos, &end, 10);
            (void)end;
            return static_cast<size_t>(len);
        }

        bool parse_interleaved_channels(const std::string &transport,
                                        uint8_t default_rtp,
                                        uint8_t default_rtcp,
                                        uint8_t &rtp,
                                        uint8_t &rtcp) {
            std::string low = lower(transport);
            if (low.find("rtp/avp/tcp") == std::string::npos) return false;
            rtp = default_rtp;
            rtcp = default_rtcp;

            size_t pos = low.find("interleaved=");
            if (pos == std::string::npos) return true;
            pos += strlen("interleaved=");

            char *end = nullptr;
            long parsed_rtp = strtol(low.c_str() + pos, &end, 10);
            if (end == low.c_str() + pos || parsed_rtp < 0 || parsed_rtp > 255) return false;
            long parsed_rtcp = parsed_rtp + 1;
            if (*end == '-') {
                char *end2 = nullptr;
                parsed_rtcp = strtol(end + 1, &end2, 10);
                if (end2 == end + 1 || parsed_rtcp < 0 || parsed_rtcp > 255) return false;
            }

            rtp = static_cast<uint8_t>(parsed_rtp);
            rtcp = static_cast<uint8_t>(parsed_rtcp);
            return true;
        }

        bool handle_request(const std::string &req) {
            std::string line = first_line(req);
            std::istringstream is(line);
            std::string method, url, version;
            is >> method >> url >> version;
            std::string cseq = header_value(req, "CSeq");
            if (cseq.empty()) cseq = "1";
            if (server_.rtsp_debug_) {
                std::string sess = session_token(header_value(req, "Session"));
                trace("REQ", method + " " + url + " cseq=" + cseq +
                             (sess.empty() ? "" : " session=" + sess));
            }

            if (method == "OPTIONS") {
                return send_response(cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n");
            }
            if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
                if (!session_id_.empty() && !require_session(req, cseq)) return true;
                std::string extra;
                if (!session_id_.empty()) extra = session_header();
                return send_response(cseq, extra);
            }
            if (method == "DESCRIBE") {
                int request_profile = server_.resolve_profile_index(url);
                if (request_profile < 0) return send_error(cseq, 404, "Not Found");
                std::string sdp;
                try {
                    sdp = server_.sdp(request_profile);
                } catch (const std::exception &e) {
                    if (server_.rtsp_debug_) trace("DESCRIBE failed", e.what());
                    return send_error(cseq, 500, "Internal Server Error");
                }
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\n"
                     << server_.response_common_headers(cseq)
                     << "Content-Type: application/sdp\r\n"
                     << "Content-Base: " << server_.content_base(url) << "\r\n"
                     << "Content-Length: " << sdp.size() << "\r\n\r\n"
                     << sdp;
                return send_raw(resp.str());
            }
            if (method == "SETUP") {
                int request_profile = server_.resolve_profile_index(url);
                if (request_profile < 0) return send_error(cseq, 404, "Not Found");
                bool audio = url.find("trackID=1") != std::string::npos;
                bool video = url.find("trackID=0") != std::string::npos;
                if (!audio && !video) return send_error(cseq, 404, "Not Found");
                if (audio && !server_.profile_audio_enabled(request_profile)) return send_error(cseq, 404, "Not Found");
                if (profile_index_ >= 0 && profile_index_ != request_profile) {
                    return send_error(cseq, 455, "Method Not Valid in This State");
                }
                if (!session_id_.empty() && !request_session_matches(req)) {
                    return send_error(cseq, 454, "Session Not Found");
                }
                profile_index_ = request_profile;

                TrackState &track = audio ? audio_track_ : video_track_;
                uint8_t default_rtp = audio ? 2 : 0;
                uint8_t default_rtcp = audio ? 3 : 1;
                uint8_t rtp_ch = default_rtp;
                uint8_t rtcp_ch = default_rtcp;
                std::string transport = header_value(req, "Transport");
                if (!parse_interleaved_channels(transport, default_rtp, default_rtcp, rtp_ch, rtcp_ch)) {
                    return send_error(cseq, 461, "Unsupported Transport");
                }

                track.setup = true;
                track.rtp_channel = rtp_ch;
                track.rtcp_channel = rtcp_ch;
                if (state_ == State::Init) state_ = State::Ready;
                if (session_id_.empty()) session_id_ = std::to_string(id_);
                std::ostringstream extra;
                extra << "Transport: RTP/AVP/TCP;unicast;interleaved="
                      << static_cast<int>(rtp_ch) << "-" << static_cast<int>(rtcp_ch) << "\r\n"
                      << session_header();
                std::ostringstream detail;
                detail << (audio ? "audio" : "video")
                       << " rtp_channel=" << static_cast<int>(rtp_ch)
                       << " rtcp_channel=" << static_cast<int>(rtcp_ch)
                       << " session=" << session_id_;
                trace("SETUP", detail.str());
                return send_response(cseq, extra.str());
            }
            if (method == "PLAY") {
                int request_profile = server_.resolve_profile_index(url);
                if (request_profile < 0) return send_error(cseq, 404, "Not Found");
                if (profile_index_ >= 0 && profile_index_ != request_profile) {
                    return send_error(cseq, 455, "Method Not Valid in This State");
                }
                if (profile_index_ < 0) profile_index_ = request_profile;
                if (!video_track_.setup && !audio_track_.setup) return send_error(cseq, 455, "Method Not Valid in This State");
                if (!require_session(req, cseq)) return true;
                bool was_playing = playing_.load(std::memory_order_relaxed);
                bool prepared_reader = false;
                bool first_reader = false;
                if (!was_playing) {
                    clear_queue();
                    if (!server_.prepare_reader_start(profile_index_, first_reader)) {
                        return send_error(cseq, 453, "Not Enough Bandwidth");
                    }
                    prepared_reader = true;
                    if (first_reader) reset_tracks_for_new_epoch();
                }
                std::ostringstream extra;
                extra << session_header()
                      << "Range: npt=now-\r\n";
                std::string base = server_.content_base(url);
                if (video_track_.setup || audio_track_.setup) {
                    extra << "RTP-Info: ";
                    bool comma = false;
                    if (video_track_.setup) {
                        extra << "url=" << base << "trackID=0"
                              << ";seq=" << video_track_.seq
                              << ";rtptime=" << (video_track_.timestamp_base + server_.video_timestamp());
                        comma = true;
                    }
                    if (audio_track_.setup) {
                        if (comma) extra << ",";
                        extra << "url=" << base << "trackID=1"
                              << ";seq=" << audio_track_.seq
                              << ";rtptime=" << (audio_track_.timestamp_base + server_.audio_timestamp());
                    }
                    extra << "\r\n";
                }
                if (!send_response(cseq, extra.str())) {
                    if (prepared_reader) server_.cancel_reader_start();
                    return false;
                }
                if (!was_playing) {
                    state_ = State::Playing;
                    playing_.store(true, std::memory_order_relaxed);
                    if (video_track_.setup) enqueue_video_parameter_sets(profile_index_, server_.video_timestamp());
                    server_.commit_reader_start();
                    if (video_track_.setup) server_.request_keyframe();
                    std::ostringstream detail;
                    detail << "profile=" << server_.profile_name(profile_index_)
                           << " tracks="
                           << (video_track_.setup ? "video" : "")
                           << (video_track_.setup && audio_track_.setup ? ",audio" : audio_track_.setup ? "audio" : "")
                           << " video_seq=" << video_track_.seq
                           << " video_rtptime=" << (video_track_.timestamp_base + server_.video_timestamp());
                    if (audio_track_.setup) {
                        detail << " audio_seq=" << audio_track_.seq
                               << " audio_rtptime=" << (audio_track_.timestamp_base + server_.audio_timestamp());
                    }
                    trace("PLAY", detail.str());
                }
                return true;
            }
            if (method == "PAUSE") {
                if (server_.resolve_profile_index(url) < 0) return send_error(cseq, 404, "Not Found");
                if (!require_session(req, cseq)) return true;
                if (playing_.exchange(false, std::memory_order_relaxed)) {
                    clear_queue();
                    server_.reader_stopped();
                }
                if (state_ == State::Playing) state_ = State::Ready;
                trace("PAUSE");
                return send_response(cseq, session_header());
            }
            if (method == "TEARDOWN") {
                if (server_.resolve_profile_index(url) < 0) return send_error(cseq, 404, "Not Found");
                if (!require_session(req, cseq)) return true;
                clear_queue();
                state_ = State::Teardown;
                trace("TEARDOWN");
                send_response(cseq, session_header());
                return false;
            }
            return send_error(cseq, 405, "Method Not Allowed");
        }

        bool send_response(const std::string &cseq, const std::string &extra) {
            std::ostringstream resp;
            resp << "RTSP/1.0 200 OK\r\n"
                 << server_.response_common_headers(cseq)
                 << extra
                 << "\r\n";
            return send_raw(resp.str());
        }

        bool send_error(const std::string &cseq, int code, const std::string &text) {
            std::ostringstream resp;
            resp << "RTSP/1.0 " << code << " " << text << "\r\n"
                 << server_.response_common_headers(cseq)
                 << "\r\n";
            return send_raw(resp.str());
        }

        std::string session_header() const {
            return "Session: " + session_id_ + ";timeout=60\r\n";
        }

        void enqueue_video_parameter_sets(int profile_index, uint32_t timestamp) {
            auto chunks = server_.video_parameter_set_chunks(profile_index, timestamp);
            if (!chunks.empty()) enqueue(chunks);
        }

        bool send_raw(const std::string &resp) {
            std::lock_guard<std::mutex> lock(send_mutex_);
            const uint8_t *data = reinterpret_cast<const uint8_t *>(resp.data());
            size_t len = resp.size();
            while (len) {
                ssize_t n = send(fd_, data, len, MSG_NOSIGNAL);
                if (n <= 0) return false;
                data += n;
                len -= static_cast<size_t>(n);
            }
            return true;
        }

        void write_loop() {
            while (!closed()) {
                RtpChunk chunk;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    queue_cv_.wait(lock, [&] { return closed() || !queue_.empty(); });
                    if (closed()) break;
                    chunk = queue_.front();
                    queue_.pop_front();
                    queue_bytes_ -= chunk.wire_size();
                }
                if (!playing()) continue;
                if (!send_rtp(chunk)) break;
            }
            closed_.store(true, std::memory_order_relaxed);
            close_socket();
        }

        bool send_rtp(const RtpChunk &chunk) {
            TrackState &track = (chunk.channel == 0) ? video_track_ : audio_track_;
            size_t payload_size = chunk.payload_size();
            size_t rtp_len = 12 + payload_size;
            if (rtp_len > 0xffff) return false;
            uint32_t rtp_timestamp = track.timestamp_base + chunk.timestamp;
            uint8_t interleaved[4] = {
                '$',
                track.rtp_channel,
                static_cast<uint8_t>(rtp_len >> 8),
                static_cast<uint8_t>(rtp_len),
            };
            uint8_t rtp[12] = {
                0x80,
                static_cast<uint8_t>((chunk.marker ? 0x80 : 0x00) | chunk.payload_type),
                static_cast<uint8_t>(track.seq >> 8),
                static_cast<uint8_t>(track.seq),
                static_cast<uint8_t>(rtp_timestamp >> 24),
                static_cast<uint8_t>(rtp_timestamp >> 16),
                static_cast<uint8_t>(rtp_timestamp >> 8),
                static_cast<uint8_t>(rtp_timestamp),
                static_cast<uint8_t>(track.ssrc >> 24),
                static_cast<uint8_t>(track.ssrc >> 16),
                static_cast<uint8_t>(track.ssrc >> 8),
                static_cast<uint8_t>(track.ssrc),
            };
            ++track.seq;

            iovec iov[4] = {
                {interleaved, sizeof(interleaved)},
                {rtp, sizeof(rtp)},
                {const_cast<uint8_t *>(chunk.prefix), chunk.prefix_size},
                {const_cast<uint8_t *>(chunk.storage.data + chunk.offset), chunk.size},
            };
            iovec *cur = iov;
            int iovcnt = 4;
            if (chunk.prefix_size == 0) {
                iov[2] = iov[3];
                iovcnt = 3;
            }
            {
                std::lock_guard<std::mutex> lock(send_mutex_);
                while (iovcnt > 0) {
                    msghdr msg{};
                    msg.msg_iov = cur;
                    msg.msg_iovlen = static_cast<size_t>(iovcnt);
                    ssize_t n = sendmsg(fd_, &msg, MSG_NOSIGNAL);
                    if (n <= 0) return false;
                    server_.send_bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
                    ssize_t left = n;
                    while (iovcnt > 0 && left >= static_cast<ssize_t>(cur[0].iov_len)) {
                        left -= static_cast<ssize_t>(cur[0].iov_len);
                        ++cur;
                        --iovcnt;
                    }
                    if (iovcnt > 0 && left > 0) {
                        cur[0].iov_base = static_cast<uint8_t *>(cur[0].iov_base) + left;
                        cur[0].iov_len -= static_cast<size_t>(left);
                    }
                }
            }
            server_.rtp_packets_.fetch_add(1, std::memory_order_relaxed);
            ++track.packet_count;
            track.octet_count += static_cast<uint32_t>(payload_size);
            track.last_rtp_timestamp = rtp_timestamp;
            return send_rtcp_if_due(track);
        }

        bool send_rtcp_if_due(TrackState &track) {
            auto now = std::chrono::steady_clock::now();
            if (track.last_rtcp != std::chrono::steady_clock::time_point::min() &&
                now - track.last_rtcp < rtcp_interval_) {
                return true;
            }
            track.last_rtcp = now;
            return send_rtcp_sender_report(track);
        }

        void send_rtcp_bye_for_setup_tracks() {
            if (video_track_.setup) (void)send_rtcp_bye(video_track_);
            if (audio_track_.setup) (void)send_rtcp_bye(audio_track_);
        }

        static void append_be32(std::vector<uint8_t> &dst, uint32_t value) {
            dst.push_back(static_cast<uint8_t>(value >> 24));
            dst.push_back(static_cast<uint8_t>(value >> 16));
            dst.push_back(static_cast<uint8_t>(value >> 8));
            dst.push_back(static_cast<uint8_t>(value));
        }

        bool send_rtcp_sender_report(const TrackState &track) {
            timeval tv{};
            gettimeofday(&tv, nullptr);
            uint32_t ntp_sec = static_cast<uint32_t>(tv.tv_sec + 2208988800UL);
            uint32_t ntp_frac = static_cast<uint32_t>(
                (static_cast<uint64_t>(tv.tv_usec) << 32) / 1000000ULL);

            std::vector<uint8_t> rtcp;
            rtcp.reserve(96);
            rtcp.push_back(0x80); // V=2, P=0, RC=0.
            rtcp.push_back(200);  // Sender Report.
            rtcp.push_back(0);
            rtcp.push_back(6);    // 28 bytes / 4 - 1.
            append_be32(rtcp, track.ssrc);
            append_be32(rtcp, ntp_sec);
            append_be32(rtcp, ntp_frac);
            append_be32(rtcp, track.last_rtp_timestamp);
            append_be32(rtcp, track.packet_count);
            append_be32(rtcp, track.octet_count);

            std::string cname = track.cname;
            if (cname.size() > 255) cname.resize(255);
            size_t sdes_start = rtcp.size();
            rtcp.push_back(0x81); // V=2, P=0, SC=1.
            rtcp.push_back(202);  // SDES.
            rtcp.push_back(0);
            rtcp.push_back(0);    // Filled after padding.
            append_be32(rtcp, track.ssrc);
            rtcp.push_back(1);    // CNAME.
            rtcp.push_back(static_cast<uint8_t>(cname.size()));
            rtcp.insert(rtcp.end(), cname.begin(), cname.end());
            rtcp.push_back(0);    // END.
            while ((rtcp.size() - sdes_start) % 4 != 0) rtcp.push_back(0);
            uint16_t sdes_length = static_cast<uint16_t>((rtcp.size() - sdes_start) / 4 - 1);
            rtcp[sdes_start + 2] = static_cast<uint8_t>(sdes_length >> 8);
            rtcp[sdes_start + 3] = static_cast<uint8_t>(sdes_length);

            if (rtcp.size() > 0xffff) return false;

            uint8_t interleaved[4] = {
                '$',
                track.rtcp_channel,
                static_cast<uint8_t>(rtcp.size() >> 8),
                static_cast<uint8_t>(rtcp.size()),
            };
            iovec iov[2] = {
                {interleaved, sizeof(interleaved)},
                {rtcp.data(), rtcp.size()},
            };
            std::lock_guard<std::mutex> lock(send_mutex_);
            return send_iov_locked(iov, 2);
        }

        bool send_rtcp_bye(const TrackState &track) {
            std::vector<uint8_t> rtcp;
            rtcp.reserve(12);
            rtcp.push_back(0x81); // V=2, P=0, SC=1.
            rtcp.push_back(203);  // BYE.
            rtcp.push_back(0);
            rtcp.push_back(1);    // 8 bytes / 4 - 1.
            append_be32(rtcp, track.ssrc);

            uint8_t interleaved[4] = {
                '$',
                track.rtcp_channel,
                static_cast<uint8_t>(rtcp.size() >> 8),
                static_cast<uint8_t>(rtcp.size()),
            };
            iovec iov[2] = {
                {interleaved, sizeof(interleaved)},
                {rtcp.data(), rtcp.size()},
            };
            std::lock_guard<std::mutex> lock(send_mutex_);
            return send_iov_locked(iov, 2);
        }

        bool send_iov_locked(iovec *iov, int iovcnt) {
            iovec *cur = iov;
            while (iovcnt > 0) {
                msghdr msg{};
                msg.msg_iov = cur;
                msg.msg_iovlen = static_cast<size_t>(iovcnt);
                ssize_t n = sendmsg(fd_, &msg, MSG_NOSIGNAL);
                if (n <= 0) return false;
                server_.send_bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
                ssize_t left = n;
                while (iovcnt > 0 && left >= static_cast<ssize_t>(cur[0].iov_len)) {
                    left -= static_cast<ssize_t>(cur[0].iov_len);
                    ++cur;
                    --iovcnt;
                }
                if (iovcnt > 0 && left > 0) {
                    cur[0].iov_base = static_cast<uint8_t *>(cur[0].iov_base) + left;
                    cur[0].iov_len -= static_cast<size_t>(left);
                }
            }
            return true;
        }

        void close_socket() {
            int fd = fd_.exchange(-1);
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
        }

        RtspServer &server_;
        std::atomic_int fd_{-1};
        uint64_t id_ = 0;
        std::atomic_bool closed_{false};
        std::atomic_bool playing_{false};
        State state_ = State::Init;
        TrackState video_track_;
        TrackState audio_track_;
        int profile_index_ = -1;
        std::string cname_;
        std::string session_id_;
        std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
        std::mutex send_mutex_;
        std::mutex queue_mutex_;
        std::condition_variable queue_cv_;
        std::deque<RtpChunk> queue_;
        size_t queue_bytes_ = 0;
        uint64_t dropped_ = 0;
        std::thread read_thread_;
        std::thread write_thread_;
    };

    const ProfileState &active_profile() const {
        int index = active_profile_index();
        if (index < 0 || index >= static_cast<int>(profiles_.size())) return profiles_[0];
        return profiles_[static_cast<size_t>(index)];
    }

    bool active_audio_enabled() const {
        if (!has_clients()) return false;
        return active_profile().profile.audio;
    }

    uint8_t video_nal_type(const ProfileState &profile, const NalUnit &nal) const {
        if (profile.profile.video_codec == VideoCodec::H264) return nal.data[0] & 0x1f;
        if (nal.size < 2) return 0xff;
        return (nal.data[0] >> 1) & 0x3f;
    }

    bool is_video_aud(const ProfileState &profile, const NalUnit &nal) const {
        uint8_t type = video_nal_type(profile, nal);
        return profile.profile.video_codec == VideoCodec::H264 ? type == 9 : type == 35;
    }

    std::vector<NalUnit> video_nals(const ProfileState &profile, const EncodedPacket &packet) const {
        if (profile.profile.video_codec == VideoCodec::H265) {
            auto parsed_nals = parse_annexb(packet.data, packet.len);
            if (!parsed_nals.empty()) return parsed_nals;
        }
        return packet_nals(packet);
    }

    void ensure_video_parameter_sets(int profile_index) {
        std::lock_guard<std::mutex> lock(profiles_mutex_);
        if (profile_index < 0 || profile_index >= static_cast<int>(profiles_.size())) {
            rtsp_die("invalid RTSP profile index");
        }
        ProfileState &profile = profiles_[static_cast<size_t>(profile_index)];
        if (profile.video_header_loaded) return;
        std::vector<uint8_t> header = profile.profile.video_header_provider();
        extract_video_parameter_sets(profile, header);
        profile.video_header_loaded = true;
    }

    void extract_video_parameter_sets(ProfileState &profile, const std::vector<uint8_t> &header) {
        for (const auto &nal : parse_annexb(header.data(), header.size())) {
            uint8_t type = video_nal_type(profile, nal);
            if (profile.profile.video_codec == VideoCodec::H264) {
                if (type == 7) profile.sps.assign(nal.data, nal.data + nal.size);
                if (type == 8) profile.pps.assign(nal.data, nal.data + nal.size);
            } else {
                if (type == 32) profile.vps.assign(nal.data, nal.data + nal.size);
                if (type == 33) profile.sps.assign(nal.data, nal.data + nal.size);
                if (type == 34) profile.pps.assign(nal.data, nal.data + nal.size);
            }
        }
        if (profile.profile.video_codec == VideoCodec::H264) {
            if (profile.sps.size() < 4 || profile.pps.empty()) rtsp_die("H.264 header did not contain SPS/PPS");
        } else {
            if (profile.vps.empty() || profile.sps.empty() || profile.pps.empty()) {
                rtsp_die("H.265 header did not contain VPS/SPS/PPS");
            }
        }
    }

    void listen_tcp() {
        std::string host;
        std::string port = listen_addr_;
        size_t colon = listen_addr_.rfind(':');
        if (colon != std::string::npos) {
            host = listen_addr_.substr(0, colon);
            port = listen_addr_.substr(colon + 1);
        }
        if (host.empty()) host = "0.0.0.0";

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *res = nullptr;
        int ret = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (ret) rtsp_die(std::string("RTSP listen getaddrinfo failed: ") + gai_strerror(ret));

        for (addrinfo *rp = res; rp; rp = rp->ai_next) {
            listen_fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (listen_fd_ < 0) continue;
            int yes = 1;
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            if (bind(listen_fd_, rp->ai_addr, rp->ai_addrlen) == 0 &&
                listen(listen_fd_, 8) == 0) {
                break;
            }
            close(listen_fd_);
            listen_fd_ = -1;
        }
        freeaddrinfo(res);
        if (listen_fd_ < 0) rtsp_die("failed to listen for RTSP clients on " + listen_addr_);
        fprintf(stderr, "RTSP server listening on %s profiles=", listen_addr_.c_str());
        for (size_t i = 0; i < profiles_.size(); ++i) {
            fprintf(stderr, "%s%s", i ? "," : "", profiles_[i].profile.path.c_str());
        }
        fprintf(stderr, " max_clients=%d\n", max_clients_);
    }

    void accept_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (!running_.load(std::memory_order_relaxed)) break;
                continue;
            }

            std::shared_ptr<Session> session;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                purge_closed_locked();
                if (static_cast<int>(sessions_.size()) >= max_clients_) {
                    close(fd);
                    continue;
                }
                session = std::make_shared<Session>(*this, fd, next_session_id_++);
                sessions_.push_back(session);
            }
            session->start();
        }
    }

    void purge_closed_locked() {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [](const std::shared_ptr<Session> &s) { return s->closed(); }),
                        sessions_.end());
    }

    void remove_session(uint64_t id) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [&](const std::shared_ptr<Session> &s) {
                                           return s->closed() || s->id() == id;
                                       }),
                        sessions_.end());
    }

    bool prepare_reader_start(int profile_index, bool &first_reader) {
        std::lock_guard<std::mutex> lock(active_mutex_);
        if (profile_index < 0 || profile_index >= static_cast<int>(profiles_.size())) return false;
        bool any_reader = active_readers_.load(std::memory_order_relaxed) > 0 || pending_readers_ > 0;
        if (any_reader && active_profile_index_ != profile_index) return false;
        first_reader = !any_reader;
        ++pending_readers_;
        if (first_reader) {
            active_profile_index_ = profile_index;
            reset_stream_timestamps();
        }
        return true;
    }

    void commit_reader_start() {
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (pending_readers_ > 0) --pending_readers_;
            active_readers_.fetch_add(1, std::memory_order_relaxed);
        }
        active_cv_.notify_all();
    }

    void request_keyframe() {
        keyframe_requested_.store(true, std::memory_order_relaxed);
    }

    std::vector<RtpChunk> video_parameter_set_chunks(int profile_index, uint32_t timestamp) {
        ensure_video_parameter_sets(profile_index);
        const ProfileState &profile = profiles_[static_cast<size_t>(profile_index)];
        std::vector<const std::vector<uint8_t> *> parameter_sets;
        if (profile.profile.video_codec == VideoCodec::H265) parameter_sets.push_back(&profile.vps);
        parameter_sets.push_back(&profile.sps);
        parameter_sets.push_back(&profile.pps);

        std::vector<RtpChunk> chunks;
        chunks.reserve(parameter_sets.size());
        for (const auto *parameter_set : parameter_sets) {
            if (!parameter_set || parameter_set->empty()) continue;
            RtpStorage storage;
            storage.bytes = std::make_shared<std::vector<uint8_t>>(*parameter_set);
            storage.data = storage.bytes->data();
            storage.size = storage.bytes->size();
            add_video_nal(profile, chunks, storage, timestamp, 0, storage.size, false);
        }
        return chunks;
    }

    void cancel_reader_start() {
        std::lock_guard<std::mutex> lock(active_mutex_);
        if (pending_readers_ > 0) --pending_readers_;
        if (pending_readers_ == 0 && active_readers_.load(std::memory_order_relaxed) == 0) {
            active_profile_index_ = -1;
        }
    }

    void reader_stopped() {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            int current = active_readers_.load(std::memory_order_relaxed);
            if (current <= 1) {
                active_readers_.store(0, std::memory_order_relaxed);
                if (pending_readers_ == 0) active_profile_index_ = -1;
                notify = true;
            } else {
                active_readers_.store(current - 1, std::memory_order_relaxed);
            }
        }
        if (notify) active_cv_.notify_all();
    }

    uint32_t random_u32() {
        uint64_t x = random_counter_.fetch_add(0x9e3779b9U, std::memory_order_relaxed);
        x ^= static_cast<uint64_t>(getpid()) << 32;
        x ^= static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return static_cast<uint32_t>(x);
    }

    void reset_stream_timestamps() {
        video_timestamp_.store(0, std::memory_order_relaxed);
        audio_timestamp_.store(0, std::memory_order_relaxed);
        audio_marker_.store(true, std::memory_order_relaxed);
    }

    void broadcast(const std::vector<RtpChunk> &chunks) {
        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            purge_closed_locked();
            sessions = sessions_;
        }
        for (const auto &session : sessions) {
            session->enqueue(chunks);
        }
    }

    void add_video_nal(const ProfileState &profile,
                       std::vector<RtpChunk> &chunks,
                       const RtpStorage &storage,
                       uint32_t timestamp,
                       size_t offset,
                       size_t len,
                       bool marker) {
        if (profile.profile.video_codec == VideoCodec::H265) {
            add_h265_nal(chunks, storage, timestamp, offset, len, marker);
            return;
        }
        add_h264_nal(chunks, storage, timestamp, offset, len, marker);
    }

    void add_h264_nal(std::vector<RtpChunk> &chunks,
                      const RtpStorage &storage,
                      uint32_t timestamp,
                      size_t offset,
                      size_t len,
                      bool marker) {
        if (len <= max_payload_) {
            RtpChunk chunk{};
            chunk.channel = 0;
            chunk.payload_type = 96;
            chunk.timestamp = timestamp;
            chunk.marker = marker;
            chunk.storage = storage;
            chunk.offset = offset;
            chunk.size = len;
            chunks.push_back(std::move(chunk));
            return;
        }

        uint8_t nal_header = storage.data[offset];
        uint8_t fu_indicator = (nal_header & 0xe0) | 28;
        uint8_t nal_type = nal_header & 0x1f;
        size_t pos = 1;
        bool start = true;
        while (pos < len) {
            size_t chunk_len = std::min(max_payload_ - 2, len - pos);
            bool end = (pos + chunk_len) >= len;
            RtpChunk chunk{};
            chunk.channel = 0;
            chunk.payload_type = 96;
            chunk.timestamp = timestamp;
            chunk.marker = marker && end;
            chunk.storage = storage;
            chunk.offset = offset + pos;
            chunk.size = chunk_len;
            chunk.prefix[0] = fu_indicator;
            chunk.prefix[1] = static_cast<uint8_t>((start ? 0x80 : 0x00) |
                                                   (end ? 0x40 : 0x00) |
                                                   nal_type);
            chunk.prefix_size = 2;
            chunks.push_back(std::move(chunk));
            pos += chunk_len;
            start = false;
        }
    }

    void add_h265_nal(std::vector<RtpChunk> &chunks,
                      const RtpStorage &storage,
                      uint32_t timestamp,
                      size_t offset,
                      size_t len,
                      bool marker) {
        if (len < 2 || offset > storage.size || len > storage.size - offset) {
            h265_invalid_nals_.fetch_add(1, std::memory_order_relaxed);
            if (rtsp_debug_) {
                fprintf(stderr, "rtsp h265 invalid nal offset=%zu len=%zu storage=%zu\n",
                        offset, len, storage.size);
            }
            return;
        }

        if (len <= max_payload_) {
            if (rtsp_debug_) h265_single_nals_.fetch_add(1, std::memory_order_relaxed);
            RtpChunk chunk{};
            chunk.channel = 0;
            chunk.payload_type = 96;
            chunk.timestamp = timestamp;
            chunk.marker = marker;
            chunk.storage = storage;
            chunk.offset = offset;
            chunk.size = len;
            chunks.push_back(std::move(chunk));
            return;
        }

        const uint8_t *nal = storage.data + offset;
        uint8_t nal_type = (nal[0] >> 1) & 0x3f;
        if (rtsp_debug_) h265_fu_nals_.fetch_add(1, std::memory_order_relaxed);
        uint8_t payload_header[2] = {
            static_cast<uint8_t>((nal[0] & 0x81) | (49 << 1)),
            nal[1],
        };
        size_t pos = 2;
        bool start = true;
        while (pos < len) {
            size_t chunk_len = std::min(max_payload_ - 3, len - pos);
            bool end = (pos + chunk_len) >= len;
            RtpChunk chunk{};
            chunk.channel = 0;
            chunk.payload_type = 96;
            chunk.timestamp = timestamp;
            chunk.marker = marker && end;
            chunk.storage = storage;
            chunk.offset = offset + pos;
            chunk.size = chunk_len;
            chunk.prefix[0] = payload_header[0];
            chunk.prefix[1] = payload_header[1];
            chunk.prefix[2] = static_cast<uint8_t>((start ? 0x80 : 0x00) |
                                                   (end ? 0x40 : 0x00) |
                                                   nal_type);
            chunk.prefix_size = 3;
            chunks.push_back(std::move(chunk));
            if (rtsp_debug_) h265_fu_packets_.fetch_add(1, std::memory_order_relaxed);
            pos += chunk_len;
            start = false;
        }
    }

    std::string sdp(int profile_index) {
        ensure_video_parameter_sets(profile_index);
        const ProfileState &profile = profiles_[static_cast<size_t>(profile_index)];
        std::string s =
            "v=0\r\n"
            "o=- 0 0 IN IP4 0.0.0.0\r\n"
            "s=rk-hdmi-streamer\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "a=type:broadcast\r\n"
            "a=control:*\r\n"
            "a=range:npt=now-\r\n"
            "m=video 0 RTP/AVP 96\r\n";
        if (profile.profile.video_codec == VideoCodec::H264) {
            s += sdp_h264(profile);
        } else {
            s += sdp_h265(profile);
        }
        if (profile.profile.audio) {
            s += "m=audio 0 RTP/AVP 97\r\n";
            if (profile.profile.audio_codec == "opus") {
                s +=
                    "a=rtpmap:97 opus/48000/2\r\n"
                    "a=fmtp:97 stereo=1;sprop-stereo=1;useinbandfec=0\r\n"
                    "a=ptime:" + audio_frame_ms_string(profile.profile.audio_frame_frames) + "\r\n"
                    "a=maxptime:" + audio_frame_ms_string(profile.profile.audio_frame_frames) + "\r\n";
            } else {
                s += "a=rtpmap:97 L16/48000/2\r\n";
            }
            s += "a=control:trackID=1\r\n";
        }
        return s;
    }

    std::string sdp_h264(const ProfileState &profile) const {
        char profile_id[7];
        snprintf(profile_id, sizeof(profile_id), "%02x%02x%02x",
                 profile.sps[1], profile.sps[2], profile.sps[3]);
        return
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=" + std::string(profile_id) +
            ";sprop-parameter-sets=" + base64_encode(profile.sps.data(), profile.sps.size()) + "," +
            base64_encode(profile.pps.data(), profile.pps.size()) + "\r\n"
            "a=control:trackID=0\r\n";
    }

    std::string sdp_h265(const ProfileState &profile) const {
        return
            "a=rtpmap:96 H265/90000\r\n"
            "a=fmtp:96 sprop-vps=" + base64_encode(profile.vps.data(), profile.vps.size()) +
            ";sprop-sps=" + base64_encode(profile.sps.data(), profile.sps.size()) +
            ";sprop-pps=" + base64_encode(profile.pps.data(), profile.pps.size()) + "\r\n"
            "a=control:trackID=0\r\n";
    }

    std::string content_base(const std::string &url) const {
        size_t track = url.find("/trackID=");
        if (track != std::string::npos) return url.substr(0, track + 1);
        if (!url.empty() && url.back() == '/') return url;
        return url + "/";
    }

    static std::string path_from_rtsp_url(const std::string &url) {
        if (url == "*") return url;
        size_t path_pos = 0;
        size_t scheme = url.find("://");
        if (scheme != std::string::npos) {
            path_pos = url.find('/', scheme + 3);
            if (path_pos == std::string::npos) return "/";
        }
        std::string path = url.substr(path_pos);
        size_t query = path.find('?');
        if (query != std::string::npos) path.resize(query);
        if (path.empty()) return "/";
        return path;
    }

    int resolve_profile_index(const std::string &url) const {
        std::string path = path_from_rtsp_url(url);
        for (size_t i = 0; i < profiles_.size(); ++i) {
            const std::string &profile_path = profiles_[i].profile.path;
            if (path == profile_path) return static_cast<int>(i);
            if (path == profile_path + "/") return static_cast<int>(i);
            std::string track_prefix = (profile_path == "/") ? "/trackID=" : profile_path + "/trackID=";
            if (path.rfind(track_prefix, 0) == 0) return static_cast<int>(i);
        }
        return -1;
    }

    bool url_matches_stream(const std::string &url) const {
        return resolve_profile_index(url) >= 0;
    }

    std::string response_common_headers(const std::string &cseq) const {
        return "CSeq: " + cseq + "\r\n"
             + "Server: rk-hdmi-streamer\r\n"
             + "Date: " + http_date() + "\r\n";
    }

    static std::string http_date() {
        time_t now = time(nullptr);
        tm tm_utc{};
        gmtime_r(&now, &tm_utc);
        char buf[64];
        strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
        return buf;
    }

    uint32_t video_timestamp() const { return video_timestamp_.load(std::memory_order_relaxed); }
    uint32_t audio_timestamp() const { return audio_timestamp_.load(std::memory_order_relaxed); }

    bool profile_audio_enabled(int profile_index) const {
        if (profile_index < 0 || profile_index >= static_cast<int>(profiles_.size())) return false;
        return profiles_[static_cast<size_t>(profile_index)].profile.audio;
    }

    std::string profile_name(int profile_index) const {
        if (profile_index < 0 || profile_index >= static_cast<int>(profiles_.size())) return "";
        return profiles_[static_cast<size_t>(profile_index)].profile.name;
    }

    std::string audio_frame_ms_string(size_t audio_frame_frames) const {
        if (audio_frame_frames == 120) return "2.5";
        return std::to_string(audio_frame_frames / 48);
    }

    std::string listen_addr_;
    std::vector<ProfileState> profiles_;
    mutable std::mutex profiles_mutex_;
    size_t max_payload_ = 1200;
    bool rtsp_debug_ = false;
    int max_clients_ = 3;
    int listen_fd_ = -1;
    std::atomic_bool running_{false};
    std::thread accept_thread_;
    std::mutex sessions_mutex_;
    std::vector<std::shared_ptr<Session>> sessions_;
    uint64_t next_session_id_ = 1;
    std::atomic_int active_readers_{0};
    std::atomic_bool keyframe_requested_{false};
    std::atomic<uint64_t> h265_single_nals_{0};
    std::atomic<uint64_t> h265_fu_nals_{0};
    std::atomic<uint64_t> h265_fu_packets_{0};
    std::atomic<uint64_t> h265_invalid_nals_{0};
    mutable std::mutex active_mutex_;
    int pending_readers_ = 0;
    int active_profile_index_ = -1;
    std::condition_variable active_cv_;
    std::atomic<uint32_t> video_timestamp_{0};
    std::atomic<uint32_t> audio_timestamp_{0};
    std::atomic_bool audio_marker_{true};
    std::atomic<uint32_t> random_counter_{0x13579bdfU};
    std::atomic<uint64_t> send_bytes_{0};
    std::atomic<uint64_t> rtp_packets_{0};
    std::atomic<uint64_t> queue_drops_{0};
    std::chrono::steady_clock::time_point stats_last_time_ = std::chrono::steady_clock::now();
    uint64_t stats_last_send_bytes_ = 0;
    uint64_t stats_last_rtp_packets_ = 0;
    uint64_t stats_last_queue_drops_ = 0;
    uint64_t stats_last_h265_single_nals_ = 0;
    uint64_t stats_last_h265_fu_nals_ = 0;
    uint64_t stats_last_h265_fu_packets_ = 0;
    uint64_t stats_last_h265_invalid_nals_ = 0;
};
