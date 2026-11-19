#include "rtsp_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "h264_utils.hpp"
#include "rtp_video.hpp"
#include "rtsp_address.hpp"
#include "rtsp_message.hpp"

namespace {

[[noreturn]] void rtsp_die(const std::string &msg) {
    throw std::runtime_error(msg);
}

inline void write_be16(uint8_t *dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v >> 8);
    dst[1] = static_cast<uint8_t>(v);
}

inline void write_be32(uint8_t *dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v >> 24);
    dst[1] = static_cast<uint8_t>(v >> 16);
    dst[2] = static_cast<uint8_t>(v >> 8);
    dst[3] = static_cast<uint8_t>(v);
}

} // namespace

class RtspServer::Impl {
private:
    using Clock = std::chrono::steady_clock;

    struct ParameterSets {
        std::vector<uint8_t> sps;
        std::vector<uint8_t> pps;
        std::vector<uint8_t> vps;
    };

    struct ProfileState {
        StreamProfile profile;
        ParameterSets parameters; // Protected by profiles_mutex_.
        // Protected by active_mutex_:
        int active_readers = 0;
        uint64_t generation = 0;
        uint32_t video_timestamp = 0;
        std::atomic_bool keyframe_requested{false};
    };

    struct RtpChunk {
        const uint8_t *data = nullptr;
        size_t size = 0;
        std::array<uint8_t, 3> prefix{};
        uint8_t prefix_size = 0;
        bool marker = false;

        size_t payload_size() const { return prefix_size + size; }
        size_t wire_size() const { return 4 + 12 + payload_size(); }
    };

    struct RawBuffer {
        uint8_t *ptr = nullptr;
        size_t len = 0;
        size_t cap = 0;

        RawBuffer() = default;
        ~RawBuffer() { delete[] ptr; }
        RawBuffer(const RawBuffer &) = delete;
        RawBuffer &operator=(const RawBuffer &) = delete;

        uint8_t *data() { return ptr; }
        const uint8_t *data() const { return ptr; }
        size_t size() const { return len; }
        size_t capacity() const { return cap; }
        void clear() { len = 0; }

        void resize_uninitialized(size_t n) {
            if (n > cap) {
                const size_t new_cap = std::max(n, cap + (cap >> 1));
                auto *p = new uint8_t[new_cap];
                delete[] ptr;
                ptr = p;
                cap = new_cap;
            }
            len = n;
        }

        void assign_bytes(const uint8_t *src, size_t n) {
            resize_uninitialized(n);
            if (n) memcpy(ptr, src, n);
        }
    };

    template <typename T>
    class RingQueue {
    public:
        bool empty() const { return count_ == 0; }
        size_t size() const { return count_; }
        T &front() { return buf_[head_]; }
        const T &front() const { return buf_[head_]; }

        void push_back(const T &v) {
            if (count_ == buf_.size()) grow();
            buf_[(head_ + count_) & (buf_.size() - 1)] = v;
            ++count_;
        }

        void pop_front() {
            head_ = (head_ + 1) & (buf_.size() - 1);
            --count_;
        }

        void clear() {
            head_ = 0;
            count_ = 0;
        }

        template <typename Fn>
        void for_each(Fn &&fn) const {
            const size_t mask = buf_.size() - 1;
            for (size_t i = 0; i < count_; ++i) fn(buf_[(head_ + i) & mask]);
        }

    private:
        void grow() {
            const size_t new_cap = buf_.empty() ? 32 : buf_.size() * 2;
            std::vector<T> next(new_cap);
            const size_t mask = buf_.size() - 1;
            for (size_t i = 0; i < count_; ++i) next[i] = buf_[(head_ + i) & mask];
            buf_.swap(next);
            head_ = 0;
        }

        std::vector<T> buf_;
        size_t head_ = 0;
        size_t count_ = 0;
    };

    struct RtpFrame {
        RawBuffer storage;
        std::vector<RtpChunk> chunks;
        size_t wire_bytes = 0;
        uint32_t timestamp = 0;
        uint32_t refcount = 0; // Mutated only on io_thread_.
        bool video = false;
        bool keyframe = false;

        void measure() {
            wire_bytes = 0;
            for (const auto &chunk : chunks) wire_bytes += chunk.wire_size();
        }

        size_t packet_count() const { return chunks.size(); }
    };

    class FramePool {
    public:
        ~FramePool() { clear(); }

        RtpFrame *acquire(bool video) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &pool = video ? video_pool_ : audio_pool_;
            if (!pool.empty()) {
                RtpFrame *f = pool.back();
                pool.pop_back();
                f->video = video;
                f->keyframe = false;
                f->timestamp = 0;
                f->wire_bytes = 0;
                f->refcount = 0;
                return f;
            }
            auto *f = new RtpFrame();
            f->video = video;
            return f;
        }

        void release(RtpFrame *frame) {
            if (!frame) return;
            const size_t cap_limit = frame->video ? kMaxVideoRecycledBytes : kMaxAudioRecycledBytes;
            if (frame->storage.capacity() > cap_limit || frame->chunks.capacity() > kMaxRecycledChunks) {
                delete frame;
                return;
            }
            frame->storage.clear();
            frame->chunks.clear();
            frame->refcount = 0;
            std::lock_guard<std::mutex> lock(mutex_);
            auto &pool = frame->video ? video_pool_ : audio_pool_;
            const size_t limit = frame->video ? kMaxVideoPoolSize : kMaxAudioPoolSize;
            if (pool.size() < limit) {
                pool.push_back(frame);
                return;
            }
            delete frame;
        }

        void clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            for (RtpFrame *f : video_pool_) delete f;
            for (RtpFrame *f : audio_pool_) delete f;
            video_pool_.clear();
            audio_pool_.clear();
        }

    private:
        static constexpr size_t kMaxVideoPoolSize = 8;
        static constexpr size_t kMaxAudioPoolSize = 8;
        static constexpr size_t kMaxVideoRecycledBytes = 256 * 1024;
        static constexpr size_t kMaxAudioRecycledBytes = 8 * 1024;
        static constexpr size_t kMaxRecycledChunks = 256;

        std::mutex mutex_;
        std::vector<RtpFrame *> video_pool_;
        std::vector<RtpFrame *> audio_pool_;
    };

    struct InboxCmd {
        enum class Type : uint8_t { Frame, AudioAll, InvalidateVideo };
        Type type = Type::Frame;
        StreamSelection selection{};
        RtpFrame *frame = nullptr;
        bool is_opus = false;
    };

    struct TrackState {
        bool setup = false;
        bool sent_any = false;
        bool rtcp_sent = false;
        uint8_t rtp_channel = 0;
        uint8_t rtcp_channel = 1;
        uint16_t seq = 0;
        uint32_t ssrc = 0;
        uint32_t timestamp_base = 0;
        uint32_t last_rtp_timestamp = 0;
        uint32_t last_rtcp_timestamp = 0;
        uint32_t packet_count = 0;
        uint32_t octet_count = 0;
        uint32_t rtcp_rx_count = 0;
    };

    struct QueuedFrame {
        RtpFrame *frame = nullptr;
        size_t charge = 0;
        bool large_keyframe = false;
    };

    enum class SessionState : uint8_t { Init, Ready, Playing, Teardown };

    struct Session {
        int fd = -1;
        uint64_t id = 0;
        SessionState state = SessionState::Init;
        int profile_index = -1;
        bool playing = false;
        bool closing = false;
        bool epoll_out = false;
        StreamSelection selection{};
        TrackState video_track{};
        TrackState audio_track{};
        std::string cname;
        std::string session_id;
        std::string rx_buf;
        size_t rx_offset = 0;
        std::vector<uint8_t> control_out;
        size_t control_offset = 0;
        std::vector<uint8_t> tx_tail;
        size_t tx_tail_offset = 0;
        RingQueue<QueuedFrame> queue;
        RtpFrame *active_frame = nullptr;
        size_t active_chunk = 0;
        size_t queue_bytes = 0;
        bool large_keyframe_queued = false;
        bool needs_video_sync = false;
        uint64_t dropped = 0;
        std::chrono::milliseconds send_deadline{2000};
        Clock::time_point last_activity = Clock::now();
        Clock::time_point stalled_since = Clock::time_point::min();
    };

    static constexpr size_t kMaxQueueBytes = 768 * 1024;
    static constexpr size_t kMaxKeyframeBytes = 3 * 1024 * 1024;
    static constexpr size_t kMaxBatchPackets = 48;
    static constexpr size_t kMaxBatchBytes = 64 * 1024;
    static constexpr uint32_t kVideoRtcpIntervalTicks = 90000 * 5;
    static constexpr uint32_t kAudioRtcpIntervalTicks = 48000 * 5;
    static constexpr auto kSessionTimeout = std::chrono::seconds(60);
    static constexpr auto kSweepInterval = std::chrono::milliseconds(250);

public:
    Impl(const std::string &listen_addr,
         std::vector<StreamProfile> profiles,
         int rtp_payload_size,
         bool rtsp_debug,
         int max_clients)
        : listen_addr_(listen_addr),
          max_payload_(static_cast<size_t>(rtp_payload_size)),
          rtsp_debug_(rtsp_debug),
          max_clients_(max_clients) {
        if (profiles.empty()) rtsp_die("RTSP server requires at least one stream");
        if (rtp_payload_size < 256 || rtp_payload_size > 60000) rtsp_die("invalid RTP payload size");
        if (max_clients <= 0) rtsp_die("RTSP server requires a positive client limit");
        profiles_.reserve(profiles.size());
        for (auto &profile : profiles) {
            if (profile.path.empty() || profile.path[0] != '/') {
                rtsp_die("RTSP stream path must start with /");
            }
            if (!profile.video && !profile.audio) {
                rtsp_die("RTSP stream must enable video or audio");
            }
            if (profile.video && !profile.video_header_provider) {
                rtsp_die("RTSP video stream requires a video header provider");
            }
            if (profile.send_timeout_ms < 500 || profile.send_timeout_ms > 60000) {
                profile.send_timeout_ms = 2000;
            }
            auto state = std::make_unique<ProfileState>();
            state->profile = std::move(profile);
            profiles_.push_back(std::move(state));
        }

        try {
            listen_tcp();
            wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (wake_fd_ < 0) rtsp_die("eventfd failed");
            epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
            if (epoll_fd_ < 0) rtsp_die("epoll_create1 failed");

            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.ptr = &listen_tag_;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
                rtsp_die("epoll_ctl add listen_fd failed");
            }
            ev.events = EPOLLIN;
            ev.data.ptr = &wake_tag_;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
                rtsp_die("epoll_ctl add wake_fd failed");
            }

            running_.store(true, std::memory_order_release);
            io_thread_ = std::thread([this] { io_loop(); });
        } catch (...) {
            if (epoll_fd_ >= 0) close(epoll_fd_);
            if (wake_fd_ >= 0) close(wake_fd_);
            if (listen_fd_ >= 0) close(listen_fd_);
            throw;
        }
    }

    ~Impl() {
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            wake_signaled_ = true;
        }
        signal_wake_fd();
        active_cv_.notify_all();
        if (io_thread_.joinable()) io_thread_.join();
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            for (auto &cmd : inbox_) {
                if (cmd.frame) pool_.release(cmd.frame);
            }
            inbox_.clear();
        }
        if (epoll_fd_ >= 0) close(epoll_fd_);
        if (wake_fd_ >= 0) close(wake_fd_);
        if (listen_fd_ >= 0) close(listen_fd_);
    }

    uint16_t port() const { return port_; }

    StreamSelection stream_selection() const {
        if (active_readers_atomic_.load(std::memory_order_acquire) == 0) return {};
        std::lock_guard<std::mutex> lock(active_mutex_);
        for (size_t i = 0; i < profiles_.size(); ++i) {
            if (profiles_[i]->active_readers > 0) {
                return {static_cast<int>(i), profiles_[i]->generation};
            }
        }
        return {};
    }

    std::vector<StreamSelection> active_streams() const {
        std::vector<StreamSelection> out;
        if (active_readers_atomic_.load(std::memory_order_acquire) == 0) return out;
        std::lock_guard<std::mutex> lock(active_mutex_);
        out.reserve(profiles_.size());
        for (size_t i = 0; i < profiles_.size(); ++i) {
            if (profiles_[i]->active_readers > 0) {
                out.push_back({static_cast<int>(i), profiles_[i]->generation});
            }
        }
        return out;
    }

    bool wait_for_clients(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(active_mutex_);
        active_cv_.wait_for(lock, timeout, [&] {
            return !running_.load(std::memory_order_acquire) || total_active_readers_ > 0;
        });
        return total_active_readers_ > 0;
    }

    bool consume_keyframe_request(int profile_index) {
        if (profile_index < 0 || static_cast<size_t>(profile_index) >= profiles_.size()) return false;
        return profiles_[static_cast<size_t>(profile_index)]->keyframe_requested.exchange(
            false, std::memory_order_relaxed);
    }

    bool consume_keyframe_request() {
        bool any = false;
        for (auto &p : profiles_) {
            if (p->keyframe_requested.exchange(false, std::memory_order_relaxed)) any = true;
        }
        return any;
    }

    void request_keyframe(int profile_index) {
        if (profile_index < 0 || static_cast<size_t>(profile_index) >= profiles_.size()) return;
        profiles_[static_cast<size_t>(profile_index)]->keyframe_requested.store(
            true, std::memory_order_relaxed);
    }

    void request_keyframe() {
        for (auto &p : profiles_) {
            if (p->profile.video) p->keyframe_requested.store(true, std::memory_order_relaxed);
        }
    }

    void invalidate_video(StreamSelection selection) {
        if (!known_selection(selection)) return;
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (!is_current_locked(selection)) return;
            inbox_.push_back({InboxCmd::Type::InvalidateVideo, selection, nullptr, false});
            need_wake = !wake_signaled_;
            wake_signaled_ = true;
        }
        if (need_wake) signal_wake_fd();
    }

    void set_video_header(StreamSelection selection, const std::vector<uint8_t> &header) {
        if (!known_selection(selection)) return;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (!is_current_locked(selection)) return;
        }
        auto &profile = *profiles_[static_cast<size_t>(selection.profile_index)];
        if (!profile.profile.video) return;
        auto parameters = extract_video_parameter_sets(profile, header);
        std::lock_guard<std::mutex> profiles_lock(profiles_mutex_);
        profile.parameters = std::move(parameters);
    }

    void write_video(const EncodedPacket &packet, StreamSelection selection) {
        if (active_readers_atomic_.load(std::memory_order_acquire) == 0) return;
        if (!known_selection(selection) || !packet.data || packet.len == 0) return;
        ProfileState &profile = *profiles_[static_cast<size_t>(selection.profile_index)];
        if (!profile.profile.video) return;

        thread_local std::vector<NalUnit> nals;
        video_nals(profile, packet, nals);
        if (nals.empty()) return;

        RtpFrame *frame = pool_.acquire(true);
        frame->keyframe = is_video_keyframe(profile, nals);
        frame->storage.assign_bytes(packet.data, packet.len);
        const uint8_t *base = frame->storage.data();

        const size_t est_chunks = nals.size() + packet.len / max_payload_ + 1;
        if (frame->chunks.capacity() < est_chunks) frame->chunks.reserve(est_chunks);

        for (const auto &nal : nals) {
            if (is_video_aud(profile, nal)) continue;
            const auto offset = static_cast<size_t>(nal.data - packet.data);
            if (offset > packet.len || nal.size > packet.len - offset) continue;
            add_video_nal(profile, frame->chunks, base + offset, nal.size);
        }
        if (frame->chunks.empty()) {
            pool_.release(frame);
            return;
        }
        frame->chunks.back().marker = true;
        frame->measure();

        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (!is_current_locked(selection)) {
                pool_.release(frame);
                return;
            }
            if (packet.has_rtp_timestamp) {
                frame->timestamp = packet.rtp_timestamp;
                profile.video_timestamp = packet.rtp_timestamp;
            } else {
                frame->timestamp = profile.video_timestamp;
                profile.video_timestamp += 90000 / profile.profile.fps;
            }
            inbox_.push_back({InboxCmd::Type::Frame, selection, frame, false});
            need_wake = !wake_signaled_;
            wake_signaled_ = true;
        }
        if (need_wake) signal_wake_fd();
    }

    void write_audio_l16(const int16_t *samples, size_t frames, StreamSelection selection) {
        if (active_readers_atomic_.load(std::memory_order_acquire) == 0) return;
        if (!known_selection(selection) || frames == 0) return;
        const StreamProfile &profile = profiles_[static_cast<size_t>(selection.profile_index)]->profile;
        if (!profile.audio || profile.audio_codec != "l16") return;
        RtpFrame *frame = build_l16_frame(samples, frames);
        publish_audio_single(frame, frames, selection);
    }

    void write_audio_opus(const uint8_t *payload, size_t len, size_t frames, StreamSelection selection) {
        if (active_readers_atomic_.load(std::memory_order_acquire) == 0) return;
        if (!known_selection(selection) || len == 0 || frames == 0) return;
        const StreamProfile &profile = profiles_[static_cast<size_t>(selection.profile_index)]->profile;
        if (!profile.audio || profile.audio_codec != "opus") return;
        RtpFrame *frame = build_opus_frame(payload, len);
        publish_audio_single(frame, frames, selection);
    }

    void write_audio_l16_all(const int16_t *samples, size_t frames) {
        if (active_readers_atomic_.load(std::memory_order_acquire) == 0 || frames == 0) return;
        RtpFrame *frame = build_l16_frame(samples, frames);
        publish_audio_all(frame, frames, false);
    }

    void write_audio_opus_all(const uint8_t *payload, size_t len, size_t frames) {
        if (active_readers_atomic_.load(std::memory_order_acquire) == 0 || len == 0 || frames == 0) return;
        RtpFrame *frame = build_opus_frame(payload, len);
        publish_audio_all(frame, frames, true);
    }

    void log_stats() {
        auto now = Clock::now();
        double secs = std::chrono::duration<double>(now - stats_last_time_).count();
        if (secs <= 0.0) return;
        uint64_t bytes = send_bytes_.load(std::memory_order_relaxed);
        uint64_t packets = rtp_packets_.load(std::memory_order_relaxed);
        uint64_t drops = queue_drops_.load(std::memory_order_relaxed);
        uint64_t d_bytes = bytes - stats_last_send_bytes_;
        uint64_t d_packets = packets - stats_last_rtp_packets_;
        uint64_t d_drops = drops - stats_last_queue_drops_;
        int clients = 0;
        bool active_h265 = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            clients = total_active_readers_;
            for (const auto &p : profiles_) {
                if (p->active_readers > 0 && p->profile.video && p->profile.video_codec == VideoCodec::H265) {
                    active_h265 = true;
                }
            }
        }
        fprintf(stderr,
                "rtsp server clients=%d net Mbps=%.2f rtp_packets/s=%.0f queue_drops=%llu\n",
                clients,
                static_cast<double>(d_bytes) * 8.0 / secs / 1000000.0,
                static_cast<double>(d_packets) / secs,
                static_cast<unsigned long long>(d_drops));
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
    RtpFrame *build_l16_frame(const int16_t *samples, size_t frames) {
        constexpr size_t channels = 2;
        const size_t sample_count = frames * channels;
        const size_t byte_len = sample_count * sizeof(int16_t);
        RtpFrame *frame = pool_.acquire(false);
        frame->storage.resize_uninitialized(byte_len);
        uint8_t *dst = frame->storage.data();
        for (size_t i = 0; i < sample_count; ++i) {
            const uint16_t be = __builtin_bswap16(static_cast<uint16_t>(samples[i]));
            memcpy(dst + i * 2, &be, 2);
        }
        frame->chunks.push_back({frame->storage.data(), byte_len, {}, 0, false});
        frame->wire_bytes = frame->chunks.front().wire_size();
        return frame;
    }

    RtpFrame *build_opus_frame(const uint8_t *payload, size_t len) {
        RtpFrame *frame = pool_.acquire(false);
        frame->storage.assign_bytes(payload, len);
        frame->chunks.push_back({frame->storage.data(), len, {}, 0, false});
        frame->wire_bytes = frame->chunks.front().wire_size();
        return frame;
    }

    void publish_audio_single(RtpFrame *frame, size_t frames, StreamSelection selection) {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (!is_current_locked(selection)) {
                pool_.release(frame);
                return;
            }
            frame->timestamp = audio_timestamp_;
            audio_timestamp_ += static_cast<uint32_t>(frames);
            inbox_.push_back({InboxCmd::Type::Frame, selection, frame, false});
            need_wake = !wake_signaled_;
            wake_signaled_ = true;
        }
        if (need_wake) signal_wake_fd();
    }

    void publish_audio_all(RtpFrame *frame, size_t frames, bool is_opus) {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (total_active_readers_ == 0) {
                pool_.release(frame);
                return;
            }
            frame->timestamp = audio_timestamp_;
            audio_timestamp_ += static_cast<uint32_t>(frames);
            inbox_.push_back({InboxCmd::Type::AudioAll, StreamSelection{}, frame, is_opus});
            need_wake = !wake_signaled_;
            wake_signaled_ = true;
        }
        if (need_wake) signal_wake_fd();
    }

    void signal_wake_fd() {
        const uint64_t one = 1;
        ssize_t n;
        do {
            n = write(wake_fd_, &one, sizeof(one));
        } while (n < 0 && errno == EINTR);
    }

    void release_frame(RtpFrame *frame) {
        if (!frame) return;
        if (--frame->refcount == 0) pool_.release(frame);
    }

    void trace(const Session &s, const char *event, const std::string &detail = {}) const {
        if (!rtsp_debug_) return;
        fprintf(stderr, "rtsp session=%llu %s%s%s\n",
                static_cast<unsigned long long>(s.id),
                event,
                detail.empty() ? "" : " ",
                detail.c_str());
    }

    void reset_track_for_new_epoch(TrackState &track, uint8_t rtp_channel, uint8_t rtcp_channel) {
        track.rtp_channel = rtp_channel;
        track.rtcp_channel = rtcp_channel;
        track.sent_any = false;
        track.rtcp_sent = false;
        track.seq = static_cast<uint16_t>(random_u32());
        track.ssrc = random_u32();
        track.timestamp_base = random_u32();
        track.last_rtp_timestamp = track.timestamp_base;
        track.last_rtcp_timestamp = track.timestamp_base;
        track.packet_count = 0;
        track.octet_count = 0;
        track.rtcp_rx_count = 0;
    }

    void set_epoll_out(Session &s, bool enable) {
        if (s.epoll_out == enable) return;
        epoll_event ev{};
        uint32_t events = EPOLLIN | EPOLLRDHUP;
        if (enable) events |= EPOLLOUT;
        ev.events = events;
        ev.data.ptr = &s;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, s.fd, &ev) == 0) {
            s.epoll_out = enable;
        }
    }

    void clear_session_media(Session &s) {
        s.queue.for_each([this](const QueuedFrame &q) { release_frame(q.frame); });
        s.queue.clear();
        if (s.active_frame) {
            release_frame(s.active_frame);
            s.active_frame = nullptr;
        }
        s.active_chunk = 0;
        s.queue_bytes = 0;
        s.large_keyframe_queued = false;
        s.needs_video_sync = false;
    }

    void stop_session_playback(Session &s) {
        if (!s.playing) return;
        const int idx = s.selection.profile_index;
        s.playing = false;
        clear_session_media(s);
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (idx >= 0 && static_cast<size_t>(idx) < profiles_.size() &&
                profiles_[static_cast<size_t>(idx)]->active_readers > 0) {
                --profiles_[static_cast<size_t>(idx)]->active_readers;
            }
            if (total_active_readers_ > 0) --total_active_readers_;
            active_readers_atomic_.store(total_active_readers_, std::memory_order_release);
        }
        active_cv_.notify_all();
    }

    void close_session(size_t index, bool send_bye) {
        auto &s = *sessions_[index];
        stop_session_playback(s);
        if (send_bye && s.tx_tail_offset == s.tx_tail.size() && s.control_offset == s.control_out.size()) {
            if (s.video_track.setup) try_send_rtcp_bye(s, s.video_track);
            if (s.audio_track.setup) try_send_rtcp_bye(s, s.audio_track);
        }
        if (rtsp_debug_) {
            std::ostringstream detail;
            detail << "video_rtp=" << s.video_track.packet_count
                   << " audio_rtp=" << s.audio_track.packet_count
                   << " video_rtcp_rx=" << s.video_track.rtcp_rx_count
                   << " audio_rtcp_rx=" << s.audio_track.rtcp_rx_count;
            trace(s, "stats", detail.str());
            trace(s, "closed");
        }
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, s.fd, nullptr);
        close(s.fd);
        s.fd = -1;
        sessions_.erase(sessions_.begin() + static_cast<ptrdiff_t>(index));
        if (sessions_.empty()) pool_.clear();
    }

    void close_session_ptr(Session *ptr, bool send_bye) {
        for (size_t i = 0; i < sessions_.size(); ++i) {
            if (sessions_[i].get() == ptr) {
                close_session(i, send_bye);
                return;
            }
        }
    }

    int compute_epoll_timeout(Clock::time_point now) const {
        if (sessions_.empty()) return -1;
        int timeout_ms = 2000;
        for (const auto &sp : sessions_) {
            if (sp->stalled_since != Clock::time_point::min()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - sp->stalled_since).count();
                const int remain = static_cast<int>(std::max<int64_t>(
                    1, sp->send_deadline.count() - elapsed));
                timeout_ms = std::min(timeout_ms, std::min(remain, 100));
            }
        }
        return timeout_ms;
    }

    void io_loop() {
        std::vector<InboxCmd> local_inbox;
        local_inbox.reserve(64);
        std::array<epoll_event, 64> events{};
        auto last_sweep = Clock::now();

        while (running_.load(std::memory_order_acquire)) {
            const auto now_before = Clock::now();
            const int timeout_ms = compute_epoll_timeout(now_before);
            const int nev = epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
            if (nev < 0) {
                if (errno == EINTR) continue;
                break;
            }

            const auto now = (nev > 0 || timeout_ms >= 0) ? Clock::now() : now_before;
            bool woke = false;

            for (int i = 0; i < nev; ++i) {
                void *ptr = events[static_cast<size_t>(i)].data.ptr;
                const uint32_t mask = events[static_cast<size_t>(i)].events;

                if (ptr == &listen_tag_) {
                    accept_pending(now);
                } else if (ptr == &wake_tag_) {
                    woke = true;
                } else {
                    auto *session = static_cast<Session *>(ptr);
                    if (!session_alive(session)) continue;

                    bool ok = true;
                    if (mask & (EPOLLERR | EPOLLHUP)) {
                        ok = false;
                    }
                    if (ok && (mask & (EPOLLIN | EPOLLRDHUP))) {
                        ok = handle_session_read(*session, now);
                    }
                    if (ok && (mask & EPOLLOUT)) {
                        ok = flush_session_output(*session, now);
                    }
                    if (!ok) {
                        close_session_ptr(session, false);
                    }
                }
            }

            if (woke) {
                drain_inbox(local_inbox, now);
            }

            if (!sessions_.empty() && now - last_sweep >= kSweepInterval) {
                last_sweep = now;
                sweep_timeouts(now);
            }
        }

        while (!sessions_.empty()) {
            close_session(sessions_.size() - 1, false);
        }
        drain_inbox_discard(local_inbox);
    }

    bool session_alive(const Session *ptr) const {
        for (const auto &sp : sessions_) {
            if (sp.get() == ptr) return true;
        }
        return false;
    }

    void accept_pending(Clock::time_point now) {
        for (;;) {
            int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }
            if (static_cast<int>(sessions_.size()) >= max_clients_) {
                close(fd);
                continue;
            }
            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef TCP_NOTSENT_LOWAT
            int lowat = 32 * 1024;
            setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
            int keepidle = 10;
            int keepintvl = 3;
            int keepcnt = 3;
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

            auto session = std::make_unique<Session>();
            session->fd = fd;
            session->id = next_session_id_++;
            session->cname = "rk-hdmi-streamer-" + std::to_string(session->id);
            session->last_activity = now;
            reset_track_for_new_epoch(session->video_track, 0, 1);
            reset_track_for_new_epoch(session->audio_track, 2, 3);

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.ptr = session.get();
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
                close(fd);
                continue;
            }
            trace(*session, "accepted");
            sessions_.push_back(std::move(session));
        }
    }

    void drain_inbox(std::vector<InboxCmd> &local_inbox, Clock::time_point now) {
        uint64_t val = 0;
        while (read(wake_fd_, &val, sizeof(val)) > 0) {}

        local_inbox.clear();
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            wake_signaled_ = false;
            local_inbox.swap(inbox_);
        }

        for (auto &cmd : local_inbox) {
            if (cmd.type == InboxCmd::Type::InvalidateVideo) {
                for (auto &sp : sessions_) {
                    auto &s = *sp;
                    if (!s.playing || s.selection != cmd.selection || !s.video_track.setup) continue;
                    drop_queued_frames(s, "capture interrupted");
                    mark_needs_video_sync(s, "capture interrupted");
                }
            } else if (cmd.type == InboxCmd::Type::AudioAll && cmd.frame) {
                RtpFrame *frame = cmd.frame;
                frame->refcount = 1;
                for (auto &sp : sessions_) {
                    auto &s = *sp;
                    if (!s.playing || !s.audio_track.setup || s.profile_index < 0) continue;
                    const auto &prof = profiles_[static_cast<size_t>(s.profile_index)]->profile;
                    if (!prof.audio) continue;
                    const bool prof_opus = (prof.audio_codec == "opus");
                    if (prof_opus != cmd.is_opus) continue;
                    enqueue_session_frame(s, frame, s.selection);
                }
                release_frame(frame);
            } else if (cmd.frame) {
                RtpFrame *frame = cmd.frame;
                frame->refcount = 1;
                for (auto &sp : sessions_) {
                    auto &s = *sp;
                    if (!s.playing || s.selection != cmd.selection) continue;
                    const TrackState &track = frame->video ? s.video_track : s.audio_track;
                    if (!track.setup) continue;
                    enqueue_session_frame(s, frame, cmd.selection);
                }
                release_frame(frame);
            }
        }
        local_inbox.clear();

        for (size_t i = 0; i < sessions_.size();) {
            auto &s = *sessions_[i];
            if (s.playing && !s.epoll_out &&
                (s.active_frame || !s.queue.empty() || s.tx_tail_offset < s.tx_tail.size() ||
                 s.control_offset < s.control_out.size())) {
                if (!flush_session_output(s, now)) {
                    trace(s, "send_failed");
                    close_session(i, false);
                    continue;
                }
            }
            ++i;
        }
    }

    void drain_inbox_discard(std::vector<InboxCmd> &local_inbox) {
        local_inbox.clear();
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            local_inbox.swap(inbox_);
        }
        for (auto &cmd : local_inbox) {
            if (cmd.frame) pool_.release(cmd.frame);
        }
        local_inbox.clear();
    }

    void sweep_timeouts(Clock::time_point now) {
        for (size_t i = 0; i < sessions_.size();) {
            auto &s = *sessions_[i];
            if (s.stalled_since != Clock::time_point::min() && now - s.stalled_since >= s.send_deadline) {
                trace(s, "send_failed");
                close_session(i, false);
                continue;
            }
            if (now - s.last_activity > kSessionTimeout) {
                trace(s, "timeout");
                close_session(i, true);
                continue;
            }
            ++i;
        }
    }

    void drop_frame(Session &s, const RtpFrame &frame, const char *reason) {
        s.dropped += frame.packet_count();
        queue_drops_.fetch_add(frame.packet_count(), std::memory_order_relaxed);
        if (frame.video) mark_needs_video_sync(s, reason);
    }

    void drop_queued_frames(Session &s, const char *reason) {
        size_t dropped = 0;
        bool dropped_video = false;
        s.queue.for_each([&](const QueuedFrame &queued) {
            dropped += queued.frame->packet_count();
            dropped_video = dropped_video || queued.frame->video;
            release_frame(queued.frame);
        });
        s.queue.clear();
        s.queue_bytes = 0;
        s.large_keyframe_queued = false;
        s.dropped += dropped;
        queue_drops_.fetch_add(dropped, std::memory_order_relaxed);
        trace(s, "queue_clear", reason);
        if (dropped_video) mark_needs_video_sync(s, reason);
    }

    void mark_needs_video_sync(Session &s, const char *reason) {
        if (!s.needs_video_sync) {
            s.needs_video_sync = true;
            trace(s, "needs_sync", reason);
            request_keyframe(s.selection.profile_index);
        }
    }

    RtpFrame *make_parameter_set_frame(int profile_index, uint32_t timestamp) {
        const auto parameters = video_parameter_sets(profile_index);
        const ProfileState &profile = *profiles_[static_cast<size_t>(profile_index)];
        const size_t total = parameters.vps.size() + parameters.sps.size() + parameters.pps.size();
        if (total == 0) return nullptr;

        RtpFrame *frame = pool_.acquire(true);
        frame->timestamp = timestamp;
        frame->storage.resize_uninitialized(total);
        size_t pos = 0;
        for (const auto *set : {&parameters.vps, &parameters.sps, &parameters.pps}) {
            if (set->empty()) continue;
            memcpy(frame->storage.data() + pos, set->data(), set->size());
            add_video_nal(profile, frame->chunks, frame->storage.data() + pos, set->size());
            pos += set->size();
        }
        frame->measure();
        if (frame->wire_bytes == 0) {
            pool_.release(frame);
            return nullptr;
        }
        return frame;
    }

    void enqueue_session_frame(Session &s, RtpFrame *frame, StreamSelection selection) {
        if (s.needs_video_sync && !frame->keyframe) return;

        const size_t media_bytes = frame->wire_bytes;
        if (media_bytes > (frame->keyframe ? kMaxKeyframeBytes : kMaxQueueBytes)) {
            drop_frame(s, *frame, "frame_too_large");
            return;
        }
        const bool large_media = media_bytes > kMaxQueueBytes;
        if ((large_media && s.large_keyframe_queued) ||
            (!large_media && s.queue_bytes + media_bytes > kMaxQueueBytes)) {
            drop_queued_frames(s, "client is not reading fast enough");
            if (s.needs_video_sync && !frame->keyframe) {
                drop_frame(s, *frame, "waiting for keyframe");
                return;
            }
        }

        RtpFrame *param_frame = nullptr;
        if (s.needs_video_sync) {
            param_frame = make_parameter_set_frame(selection.profile_index, frame->timestamp);
        }
        const size_t parameter_bytes = param_frame ? param_frame->wire_bytes : 0;
        const bool large_keyframe = media_bytes + parameter_bytes > kMaxQueueBytes;
        if (parameter_bytes > kMaxQueueBytes || media_bytes + parameter_bytes > kMaxKeyframeBytes) {
            if (param_frame) pool_.release(param_frame);
            drop_frame(s, *frame, "keyframe and parameter sets too large");
            return;
        }
        const size_t media_charge = large_keyframe ? 0 : media_bytes;
        if ((large_keyframe && s.large_keyframe_queued) ||
            s.queue_bytes + media_charge + parameter_bytes > kMaxQueueBytes) {
            drop_queued_frames(s, "making room for keyframe");
        }
        if (param_frame) {
            param_frame->refcount = 1;
            s.queue.push_back({param_frame, parameter_bytes, false});
            s.queue_bytes += parameter_bytes;
        }
        ++frame->refcount;
        s.queue.push_back({frame, media_charge, large_keyframe});
        s.queue_bytes += media_charge;
        s.large_keyframe_queued |= large_keyframe;
        if (s.needs_video_sync) {
            s.needs_video_sync = false;
            trace(s, "resync", "keyframe accepted");
        }
    }

    bool handle_session_read(Session &s, Clock::time_point now) {
        char buf[4096];
        for (;;) {
            const ssize_t n = recv(s.fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return false;
            }
            if (n == 0) return false;
            s.rx_buf.append(buf, static_cast<size_t>(n));
            if (s.rx_buf.size() - s.rx_offset > rtsp::max_message_size) return false;
        }

        while (s.rx_offset < s.rx_buf.size()) {
            const std::string_view pending(s.rx_buf.data() + s.rx_offset, s.rx_buf.size() - s.rx_offset);
            if (pending[0] == '$') {
                if (pending.size() < 4) break;
                const size_t frame_len =
                    (static_cast<uint8_t>(pending[2]) << 8) | static_cast<uint8_t>(pending[3]);
                if (pending.size() < 4 + frame_len) break;
                handle_interleaved_frame(s, static_cast<uint8_t>(pending[1]),
                                         reinterpret_cast<const uint8_t *>(pending.data() + 4),
                                         frame_len);
                s.last_activity = now;
                s.rx_offset += 4 + frame_len;
                continue;
            }

            const size_t hdr_end = pending.find("\r\n\r\n");
            if (hdr_end == std::string_view::npos) break;
            const size_t header_len = hdr_end + 4;
            const auto body_len = rtsp::body_size(pending.substr(0, header_len));
            if (!body_len) return false;
            const size_t message_len = header_len + *body_len;
            if (pending.size() < message_len) break;

            const std::string_view req = pending.substr(0, message_len);
            s.last_activity = now;
            const bool keep_open = handle_request(s, req);
            s.rx_offset += message_len;
            if (!keep_open) {
                s.closing = true;
                break;
            }
        }

        if (s.rx_offset == s.rx_buf.size()) {
            s.rx_buf.clear();
            s.rx_offset = 0;
        } else if (s.rx_offset > 4096) {
            s.rx_buf.erase(0, s.rx_offset);
            s.rx_offset = 0;
        }

        return flush_session_output(s, now);
    }

    void handle_interleaved_frame(Session &s, uint8_t channel, const uint8_t *data, size_t len) {
        if (len < 2) return;
        TrackState *track = nullptr;
        if (s.video_track.setup && channel == s.video_track.rtcp_channel) track = &s.video_track;
        if (s.audio_track.setup && channel == s.audio_track.rtcp_channel) track = &s.audio_track;
        if (!track) return;
        const uint8_t rtcp_type = data[1];
        if (rtcp_type >= 192 && rtcp_type <= 223) ++track->rtcp_rx_count;
    }

    bool request_session_matches(const Session &s, std::string_view req) const {
        if (s.session_id.empty()) return false;
        return rtsp::session_token(rtsp::header_value(req, "Session")) == s.session_id;
    }

    void queue_control(Session &s, std::string_view data) {
        s.control_out.insert(s.control_out.end(),
                             reinterpret_cast<const uint8_t *>(data.data()),
                             reinterpret_cast<const uint8_t *>(data.data()) + data.size());
    }

    void queue_response(Session &s, const std::string &cseq, const std::string &extra) {
        std::string resp;
        resp.reserve(160 + extra.size());
        resp += "RTSP/1.0 200 OK\r\n";
        resp += response_common_headers(cseq);
        resp += extra;
        resp += "\r\n";
        queue_control(s, resp);
    }

    void queue_error(Session &s, const std::string &cseq, int code, const char *text) {
        std::string resp;
        resp.reserve(160);
        resp += "RTSP/1.0 ";
        resp += std::to_string(code);
        resp += ' ';
        resp += text;
        resp += "\r\n";
        resp += response_common_headers(cseq);
        resp += "\r\n";
        queue_control(s, resp);
    }

    static std::string session_header(const Session &s) {
        return "Session: " + s.session_id + ";timeout=60\r\n";
    }

    static bool parse_interleaved_channels(std::string_view transport,
                                           uint8_t default_rtp,
                                           uint8_t default_rtcp,
                                           uint8_t &rtp,
                                           uint8_t &rtcp) {
        std::string low(transport);
        for (char &c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
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

        if (parsed_rtcp > 255 || parsed_rtp == parsed_rtcp) return false;
        rtp = static_cast<uint8_t>(parsed_rtp);
        rtcp = static_cast<uint8_t>(parsed_rtcp);
        return true;
    }

    static bool setup_track(Session &s, bool audio, uint8_t rtp_channel, uint8_t rtcp_channel) {
        TrackState &track = audio ? s.audio_track : s.video_track;
        const TrackState &other = audio ? s.video_track : s.audio_track;
        if (other.setup && (rtp_channel == other.rtp_channel || rtp_channel == other.rtcp_channel ||
                            rtcp_channel == other.rtp_channel || rtcp_channel == other.rtcp_channel)) {
            return false;
        }
        track.setup = true;
        track.rtp_channel = rtp_channel;
        track.rtcp_channel = rtcp_channel;
        return true;
    }

    bool handle_request(Session &s, std::string_view req) {
        const std::string line(rtsp::first_line(req));
        std::istringstream is(line);
        std::string method, url, version;
        is >> method >> url >> version;
        std::string cseq(rtsp::header_value(req, "CSeq"));
        if (cseq.empty()) cseq = "1";
        if (rtsp_debug_) {
            std::string sess(rtsp::session_token(rtsp::header_value(req, "Session")));
            trace(s, "REQ", method + " " + url + " cseq=" + cseq +
                            (sess.empty() ? "" : " session=" + sess));
        }

        if (method.empty() || url.empty() || version != "RTSP/1.0") {
            queue_error(s, cseq, 400, "Bad Request");
            return true;
        }

        if (method == "OPTIONS") {
            queue_response(s, cseq,
                "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n");
            return true;
        }
        if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
            if (!s.session_id.empty() && !request_session_matches(s, req)) {
                queue_error(s, cseq, 454, "Session Not Found");
                return true;
            }
            queue_response(s, cseq, s.session_id.empty() ? std::string{} : session_header(s));
            return true;
        }
        if (method == "DESCRIBE") {
            const int request_profile = resolve_profile_index(url);
            if (request_profile < 0) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            std::string body;
            try {
                body = sdp(request_profile);
            } catch (const std::exception &e) {
                if (rtsp_debug_) trace(s, "DESCRIBE failed", e.what());
                queue_error(s, cseq, 500, "Internal Server Error");
                return true;
            }
            std::ostringstream resp;
            resp << "RTSP/1.0 200 OK\r\n"
                 << response_common_headers(cseq)
                 << "Content-Type: application/sdp\r\n"
                 << "Content-Base: " << content_base(url) << "\r\n"
                 << "Content-Length: " << body.size() << "\r\n\r\n"
                 << body;
            queue_control(s, resp.str());
            return true;
        }
        if (method == "SETUP") {
            const int request_profile = resolve_profile_index(url);
            if (request_profile < 0) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            if (s.state == SessionState::Playing) {
                queue_error(s, cseq, 455, "Method Not Valid in This State");
                return true;
            }
            const std::string path = path_from_rtsp_url(url);
            const auto track_pos = path.rfind('/');
            const auto track_name = path.substr(track_pos == std::string::npos ? 0 : track_pos + 1);
            const bool audio = track_name == "trackID=1";
            const bool video = track_name == "trackID=0";
            if (!audio && !video) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            const auto &prof = profiles_[static_cast<size_t>(request_profile)]->profile;
            if (video && !prof.video) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            if (audio && !prof.audio) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            if (s.profile_index >= 0 && s.profile_index != request_profile) {
                queue_error(s, cseq, 455, "Method Not Valid in This State");
                return true;
            }
            if (!s.session_id.empty() && !request_session_matches(s, req)) {
                queue_error(s, cseq, 454, "Session Not Found");
                return true;
            }

            const uint8_t default_rtp = audio ? 2 : 0;
            const uint8_t default_rtcp = audio ? 3 : 1;
            uint8_t rtp_ch = default_rtp;
            uint8_t rtcp_ch = default_rtcp;
            if (!parse_interleaved_channels(rtsp::header_value(req, "Transport"),
                                            default_rtp, default_rtcp, rtp_ch, rtcp_ch)) {
                queue_error(s, cseq, 461, "Unsupported Transport");
                return true;
            }
            if (!setup_track(s, audio, rtp_ch, rtcp_ch)) {
                queue_error(s, cseq, 461, "Interleaved Channels In Use");
                return true;
            }
            s.profile_index = request_profile;
            s.send_deadline = std::chrono::milliseconds(prof.send_timeout_ms);
            if (s.state == SessionState::Init) s.state = SessionState::Ready;
            if (s.session_id.empty()) s.session_id = std::to_string(s.id);
            std::ostringstream extra;
            extra << "Transport: RTP/AVP/TCP;unicast;interleaved="
                  << static_cast<int>(rtp_ch) << "-" << static_cast<int>(rtcp_ch) << "\r\n"
                  << session_header(s);
            if (rtsp_debug_) {
                std::ostringstream detail;
                detail << (audio ? "audio" : "video")
                       << " rtp_channel=" << static_cast<int>(rtp_ch)
                       << " rtcp_channel=" << static_cast<int>(rtcp_ch)
                       << " session=" << s.session_id;
                trace(s, "SETUP", detail.str());
            }
            queue_response(s, cseq, extra.str());
            return true;
        }
        if (method == "PLAY") {
            const int request_profile = resolve_profile_index(url);
            if (request_profile < 0) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            if (s.profile_index >= 0 && s.profile_index != request_profile) {
                queue_error(s, cseq, 455, "Method Not Valid in This State");
                return true;
            }
            if (!request_session_matches(s, req)) {
                queue_error(s, cseq, 454, "Session Not Found");
                return true;
            }
            const bool video_setup = s.video_track.setup;
            const bool audio_setup = s.audio_track.setup;
            if (!video_setup && !audio_setup) {
                queue_error(s, cseq, 455, "Method Not Valid in This State");
                return true;
            }
            if (!s.playing && video_setup) {
                try {
                    video_parameter_sets(s.profile_index);
                } catch (const std::exception &e) {
                    trace(s, "PLAY failed", e.what());
                    queue_error(s, cseq, 500, "Internal Server Error");
                    return true;
                }
            }

            bool first_reader_for_profile = false;
            StreamSelection selection;
            std::pair<uint32_t, uint32_t> ts{0, 0};
            if (!s.playing) {
                std::lock_guard<std::mutex> lock(active_mutex_);
                auto &ps = *profiles_[static_cast<size_t>(s.profile_index)];
                first_reader_for_profile = (ps.active_readers == 0);
                if (first_reader_for_profile) {
                    ++ps.generation;
                    ps.video_timestamp = 0;
                }
                if (total_active_readers_ == 0) {
                    audio_timestamp_ = 0;
                }
                selection = {s.profile_index, ps.generation};
                ts = {ps.video_timestamp, audio_timestamp_};
                ++ps.active_readers;
                ++total_active_readers_;
                active_readers_atomic_.store(total_active_readers_, std::memory_order_release);
            } else {
                std::lock_guard<std::mutex> lock(active_mutex_);
                ts = {profiles_[static_cast<size_t>(s.profile_index)]->video_timestamp, audio_timestamp_};
            }

            if (first_reader_for_profile) {
                reset_track_for_new_epoch(s.video_track, s.video_track.rtp_channel, s.video_track.rtcp_channel);
                reset_track_for_new_epoch(s.audio_track, s.audio_track.rtp_channel, s.audio_track.rtcp_channel);
            }

            std::ostringstream extra;
            extra << session_header(s) << "Range: npt=now-\r\n";
            const std::string base = content_base(url);
            extra << "RTP-Info: ";
            if (video_setup) {
                extra << "url=" << base << "trackID=0"
                      << ";seq=" << s.video_track.seq
                      << ";rtptime=" << (s.video_track.timestamp_base + ts.first);
            }
            if (audio_setup) {
                if (video_setup) extra << ",";
                extra << "url=" << base << "trackID=1"
                      << ";seq=" << s.audio_track.seq
                      << ";rtptime=" << (s.audio_track.timestamp_base + ts.second);
            }
            extra << "\r\n";
            queue_response(s, cseq, extra.str());

            if (selection) {
                clear_session_media(s);
                s.needs_video_sync = video_setup;
                s.selection = selection;
                s.playing = true;
                s.state = SessionState::Playing;
                active_cv_.notify_all();
                if (video_setup) request_keyframe(selection.profile_index);
                trace(s, "PLAY", profile_name(selection.profile_index));
            }
            return true;
        }
        if (method == "PAUSE") {
            const int request_profile = resolve_profile_index(url);
            if (request_profile < 0) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            if (s.profile_index >= 0 && request_profile != s.profile_index) {
                queue_error(s, cseq, 455, "Method Not Valid in This State");
                return true;
            }
            if (!request_session_matches(s, req)) {
                queue_error(s, cseq, 454, "Session Not Found");
                return true;
            }
            stop_session_playback(s);
            if (s.state == SessionState::Playing) s.state = SessionState::Ready;
            trace(s, "PAUSE");
            queue_response(s, cseq, session_header(s));
            return true;
        }
        if (method == "TEARDOWN") {
            const int request_profile = resolve_profile_index(url);
            if (request_profile < 0) {
                queue_error(s, cseq, 404, "Not Found");
                return true;
            }
            if (s.profile_index >= 0 && request_profile != s.profile_index) {
                queue_error(s, cseq, 455, "Method Not Valid in This State");
                return true;
            }
            if (!request_session_matches(s, req)) {
                queue_error(s, cseq, 454, "Session Not Found");
                return true;
            }
            stop_session_playback(s);
            s.state = SessionState::Teardown;
            trace(s, "TEARDOWN");
            queue_response(s, cseq, session_header(s));
            if (s.video_track.setup) append_rtcp_bye(s.control_out, s.video_track);
            if (s.audio_track.setup) append_rtcp_bye(s.control_out, s.audio_track);
            return false;
        }
        queue_error(s, cseq, 405, "Method Not Allowed");
        return true;
    }

    enum class DrainResult : uint8_t { Done, WouldBlock, Error };

    DrainResult drain_buffer(Session &s, std::vector<uint8_t> &buf, size_t &offset, Clock::time_point now) {
        while (offset < buf.size()) {
            const ssize_t n = send(s.fd, buf.data() + offset, buf.size() - offset,
                                   MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (s.stalled_since == Clock::time_point::min()) s.stalled_since = now;
                    set_epoll_out(s, true);
                    return DrainResult::WouldBlock;
                }
                return DrainResult::Error;
            }
            if (n == 0) return DrainResult::Error;
            send_bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            offset += static_cast<size_t>(n);
        }
        buf.clear();
        offset = 0;
        if (buf.capacity() > 128 * 1024) std::vector<uint8_t>().swap(buf);
        s.stalled_since = Clock::time_point::min();
        return DrainResult::Done;
    }

    bool flush_session_output(Session &s, Clock::time_point now) {
        if (s.tx_tail_offset < s.tx_tail.size()) {
            const auto r = drain_buffer(s, s.tx_tail, s.tx_tail_offset, now);
            if (r == DrainResult::Error) return false;
            if (r == DrainResult::WouldBlock) return true;
        }
        if (s.control_offset < s.control_out.size()) {
            const auto r = drain_buffer(s, s.control_out, s.control_offset, now);
            if (r == DrainResult::Error) return false;
            if (r == DrainResult::WouldBlock) return true;
        }
        if (s.closing) return false;
        if (!s.playing) {
            set_epoll_out(s, false);
            return true;
        }

        while (s.active_frame || !s.queue.empty()) {
            if (!s.active_frame) {
                auto q = s.queue.front();
                s.queue.pop_front();
                s.queue_bytes -= q.charge;
                if (q.large_keyframe) s.large_keyframe_queued = false;
                s.active_frame = q.frame;
                s.active_chunk = 0;
            }

            const RtpFrame &frame = *s.active_frame;
            const bool is_video = frame.video;
            TrackState &track = is_video ? s.video_track : s.audio_track;
            if (!track.setup) {
                release_frame(s.active_frame);
                s.active_frame = nullptr;
                s.active_chunk = 0;
                continue;
            }

            size_t count = 1;
            size_t batch_bytes = frame.chunks[s.active_chunk].wire_size();
            while (count < kMaxBatchPackets && s.active_chunk + count < frame.chunks.size()) {
                const size_t next_bytes = frame.chunks[s.active_chunk + count].wire_size();
                if (batch_bytes + next_bytes > kMaxBatchBytes) break;
                batch_bytes += next_bytes;
                ++count;
            }

            const uint8_t payload_type = is_video ? 96 : 97;
            const uint8_t channel = track.rtp_channel;
            const uint32_t timestamp = track.timestamp_base + frame.timestamp;
            const uint32_t ssrc = track.ssrc;
            uint16_t seq = track.seq;
            const bool audio_start_marker = !is_video && !track.sent_any;

            std::array<std::array<uint8_t, 19>, kMaxBatchPackets> headers;
            std::array<iovec, 2 * kMaxBatchPackets> iov;
            uint32_t octets = 0;
            for (size_t i = 0; i < count; ++i, ++seq) {
                const auto &chunk = frame.chunks[s.active_chunk + i];
                const size_t payload_size = chunk.payload_size();
                const size_t rtp_len = 12 + payload_size;
                if (rtp_len > 0xffff) return false;
                const bool marker = chunk.marker || (audio_start_marker && i == 0);
                auto &h = headers[i];
                h[0] = '$';
                h[1] = channel;
                write_be16(&h[2], static_cast<uint16_t>(rtp_len));
                h[4] = 0x80;
                h[5] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | payload_type);
                write_be16(&h[6], seq);
                write_be32(&h[8], timestamp);
                write_be32(&h[12], ssrc);
                h[16] = chunk.prefix[0];
                h[17] = chunk.prefix[1];
                h[18] = chunk.prefix[2];
                iov[2 * i] = {h.data(), 16 + static_cast<size_t>(chunk.prefix_size)};
                iov[2 * i + 1] = {const_cast<uint8_t *>(chunk.data), chunk.size};
                octets += static_cast<uint32_t>(payload_size);
            }

            const bool more = (s.active_chunk + count < frame.chunks.size()) || !s.queue.empty();
            msghdr msg{};
            msg.msg_iov = iov.data();
            msg.msg_iovlen = 2 * count;
            ssize_t n;
            do {
                n = sendmsg(s.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT | (more ? MSG_MORE : 0));
            } while (n < 0 && errno == EINTR);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (s.stalled_since == Clock::time_point::min()) s.stalled_since = now;
                    set_epoll_out(s, true);
                    return true;
                }
                return false;
            }
            if (n == 0) return false;

            send_bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            rtp_packets_.fetch_add(count, std::memory_order_relaxed);
            track.sent_any = true;
            track.seq = static_cast<uint16_t>(track.seq + count);
            track.packet_count += static_cast<uint32_t>(count);
            track.octet_count += octets;
            track.last_rtp_timestamp = timestamp;
            s.active_chunk += count;

            const bool finished_frame = (s.active_chunk >= frame.chunks.size());
            const bool partial = static_cast<size_t>(n) < batch_bytes;
            if (partial) {
                const size_t unsent = batch_bytes - static_cast<size_t>(n);
                s.tx_tail.clear();
                s.tx_tail.reserve(unsent);
                size_t skip = static_cast<size_t>(n);
                for (size_t i = 0; i < 2 * count; ++i) {
                    if (skip >= iov[i].iov_len) {
                        skip -= iov[i].iov_len;
                        continue;
                    }
                    const auto *base = static_cast<const uint8_t *>(iov[i].iov_base) + skip;
                    const size_t len = iov[i].iov_len - skip;
                    s.tx_tail.insert(s.tx_tail.end(), base, base + len);
                    skip = 0;
                }
                s.tx_tail_offset = 0;
                if (finished_frame) {
                    release_frame(s.active_frame);
                    s.active_frame = nullptr;
                    s.active_chunk = 0;
                }
                if (s.stalled_since == Clock::time_point::min()) s.stalled_since = now;
                set_epoll_out(s, true);
                return true;
            }

            s.stalled_since = Clock::time_point::min();
            if (finished_frame) {
                release_frame(s.active_frame);
                s.active_frame = nullptr;
                s.active_chunk = 0;
            }

            const uint32_t rtcp_interval = is_video ? kVideoRtcpIntervalTicks : kAudioRtcpIntervalTicks;
            if (!track.rtcp_sent ||
                static_cast<uint32_t>(timestamp - track.last_rtcp_timestamp) >= rtcp_interval) {
                track.rtcp_sent = true;
                track.last_rtcp_timestamp = timestamp;
                if (!send_rtcp_sender_report(s, track, now)) return false;
                if (s.tx_tail_offset < s.tx_tail.size()) return true;
            }
        }

        set_epoll_out(s, false);
        return true;
    }

    bool send_rtcp_sender_report(Session &s, const TrackState &track, Clock::time_point now) {
        timeval tv{};
        gettimeofday(&tv, nullptr);
        const uint32_t ntp_sec = static_cast<uint32_t>(tv.tv_sec + 2208988800UL);
        const uint32_t ntp_frac = static_cast<uint32_t>(
            (static_cast<uint64_t>(tv.tv_usec) << 32) / 1000000ULL);

        uint8_t pkt[4 + 28 + 268];
        size_t pos = 4;
        pkt[pos++] = 0x80; // V=2, P=0, RC=0.
        pkt[pos++] = 200;  // Sender Report.
        pkt[pos++] = 0;
        pkt[pos++] = 6;    // 28 / 4 - 1.
        write_be32(&pkt[pos], track.ssrc); pos += 4;
        write_be32(&pkt[pos], ntp_sec); pos += 4;
        write_be32(&pkt[pos], ntp_frac); pos += 4;
        write_be32(&pkt[pos], track.last_rtp_timestamp); pos += 4;
        write_be32(&pkt[pos], track.packet_count); pos += 4;
        write_be32(&pkt[pos], track.octet_count); pos += 4;

        const size_t cname_len = std::min<size_t>(s.cname.size(), 255);
        const size_t sdes_start = pos;
        pkt[pos++] = 0x81; // V=2, P=0, SC=1.
        pkt[pos++] = 202;  // SDES.
        pkt[pos++] = 0;
        pkt[pos++] = 0;
        write_be32(&pkt[pos], track.ssrc); pos += 4;
        pkt[pos++] = 1; // CNAME.
        pkt[pos++] = static_cast<uint8_t>(cname_len);
        memcpy(&pkt[pos], s.cname.data(), cname_len);
        pos += cname_len;
        pkt[pos++] = 0; // END.
        while ((pos - sdes_start) % 4 != 0) pkt[pos++] = 0;
        const uint16_t sdes_length = static_cast<uint16_t>((pos - sdes_start) / 4 - 1);
        write_be16(&pkt[sdes_start + 2], sdes_length);

        const size_t rtcp_len = pos - 4;
        pkt[0] = '$';
        pkt[1] = track.rtcp_channel;
        write_be16(&pkt[2], static_cast<uint16_t>(rtcp_len));

        ssize_t n;
        do {
            n = send(s.fd, pkt, pos, MSG_NOSIGNAL | MSG_DONTWAIT);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                s.tx_tail.assign(pkt, pkt + pos);
                s.tx_tail_offset = 0;
                if (s.stalled_since == Clock::time_point::min()) s.stalled_since = now;
                set_epoll_out(s, true);
                return true;
            }
            return false;
        }
        if (n == 0) return false;
        send_bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        if (static_cast<size_t>(n) < pos) {
            s.tx_tail.assign(pkt + n, pkt + pos);
            s.tx_tail_offset = 0;
            if (s.stalled_since == Clock::time_point::min()) s.stalled_since = now;
            set_epoll_out(s, true);
        }
        return true;
    }

    static void append_rtcp_bye(std::vector<uint8_t> &dst, const TrackState &track) {
        uint8_t pkt[12] = {'$', track.rtcp_channel, 0, 8, 0x81, 203, 0, 1, 0, 0, 0, 0};
        write_be32(&pkt[8], track.ssrc);
        dst.insert(dst.end(), pkt, pkt + sizeof(pkt));
    }

    void try_send_rtcp_bye(Session &s, const TrackState &track) {
        uint8_t pkt[12] = {'$', track.rtcp_channel, 0, 8, 0x81, 203, 0, 1, 0, 0, 0, 0};
        write_be32(&pkt[8], track.ssrc);
        ssize_t n;
        do {
            n = send(s.fd, pkt, sizeof(pkt), MSG_NOSIGNAL | MSG_DONTWAIT);
        } while (n < 0 && errno == EINTR);
        if (n > 0) send_bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    }

    bool known_selection(StreamSelection selection) const {
        return selection && static_cast<size_t>(selection.profile_index) < profiles_.size();
    }

    bool is_current_locked(StreamSelection selection) const {
        if (!known_selection(selection)) return false;
        const auto &ps = *profiles_[static_cast<size_t>(selection.profile_index)];
        return ps.active_readers > 0 && ps.generation == selection.generation;
    }

    uint8_t video_nal_type(const ProfileState &profile, const NalUnit &nal) const {
        if (profile.profile.video_codec == VideoCodec::H264) return nal.data[0] & 0x1f;
        if (nal.size < 2) return 0xff;
        return (nal.data[0] >> 1) & 0x3f;
    }

    bool is_video_aud(const ProfileState &profile, const NalUnit &nal) const {
        const uint8_t type = video_nal_type(profile, nal);
        return profile.profile.video_codec == VideoCodec::H264 ? type == 9 : type == 35;
    }

    void video_nals(const ProfileState &profile, const EncodedPacket &packet,
                    std::vector<NalUnit> &out) const {
        if (profile.profile.video_codec == VideoCodec::H265) {
            parse_annexb(packet.data, packet.len, out);
            if (!out.empty()) return;
        }
        packet_nals(packet, out);
    }

    ParameterSets video_parameter_sets(int profile_index) {
        std::function<std::vector<uint8_t>()> provider;
        {
            std::lock_guard<std::mutex> lock(profiles_mutex_);
            auto &profile = *profiles_.at(static_cast<size_t>(profile_index));
            if (!profile.parameters.sps.empty()) return profile.parameters;
            provider = profile.profile.video_header_provider;
        }
        auto header = provider();
        const auto &profile = *profiles_.at(static_cast<size_t>(profile_index));
        auto params = extract_video_parameter_sets(profile, header);
        std::lock_guard<std::mutex> lock(profiles_mutex_);
        auto &mutable_profile = *profiles_.at(static_cast<size_t>(profile_index));
        if (mutable_profile.parameters.sps.empty()) {
            mutable_profile.parameters = params;
        }
        return mutable_profile.parameters;
    }

    ParameterSets extract_video_parameter_sets(const ProfileState &profile,
                                               const std::vector<uint8_t> &header) const {
        ParameterSets parameters;
        for (const auto &nal : parse_annexb(header.data(), header.size())) {
            const uint8_t type = video_nal_type(profile, nal);
            if (profile.profile.video_codec == VideoCodec::H264) {
                if (type == 7) parameters.sps.assign(nal.data, nal.data + nal.size);
                if (type == 8) parameters.pps.assign(nal.data, nal.data + nal.size);
            } else {
                if (type == 32) parameters.vps.assign(nal.data, nal.data + nal.size);
                if (type == 33) parameters.sps.assign(nal.data, nal.data + nal.size);
                if (type == 34) parameters.pps.assign(nal.data, nal.data + nal.size);
            }
        }
        if (profile.profile.video_codec == VideoCodec::H264) {
            if (parameters.sps.size() < 4 || parameters.pps.empty()) {
                rtsp_die("H.264 header did not contain SPS/PPS");
            }
        } else {
            if (parameters.vps.empty() || parameters.sps.empty() || parameters.pps.empty()) {
                rtsp_die("H.265 header did not contain VPS/SPS/PPS");
            }
        }
        return parameters;
    }

    void listen_tcp() {
        const auto address = rtsp::listen_address(listen_addr_);
        if (!address) rtsp_die("invalid RTSP listen address");
        const std::string host(address->host);
        const std::string port(address->port);

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *res = nullptr;
        int ret = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (ret) rtsp_die(std::string("RTSP listen getaddrinfo failed: ") + gai_strerror(ret));

        for (addrinfo *rp = res; rp; rp = rp->ai_next) {
            listen_fd_ = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                rp->ai_protocol);
            if (listen_fd_ < 0) continue;
            int yes = 1;
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            if (bind(listen_fd_, rp->ai_addr, rp->ai_addrlen) == 0 && listen(listen_fd_, 16) == 0) {
                break;
            }
            close(listen_fd_);
            listen_fd_ = -1;
        }
        freeaddrinfo(res);
        if (listen_fd_ < 0) rtsp_die("failed to listen for RTSP clients on " + listen_addr_);

        sockaddr_storage addr{};
        socklen_t address_size = sizeof(addr);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &address_size) == 0) {
            if (addr.ss_family == AF_INET) port_ = ntohs(reinterpret_cast<sockaddr_in *>(&addr)->sin_port);
            if (addr.ss_family == AF_INET6) port_ = ntohs(reinterpret_cast<sockaddr_in6 *>(&addr)->sin6_port);
        }
        fprintf(stderr, "RTSP server listening on %s streams=", listen_addr_.c_str());
        for (size_t i = 0; i < profiles_.size(); ++i) {
            fprintf(stderr, "%s%s", i ? "," : "", profiles_[i]->profile.path.c_str());
        }
        fprintf(stderr, " max_clients=%d\n", max_clients_);
    }

    uint32_t random_u32() {
        uint64_t x = random_counter_.fetch_add(0x9e3779b9U, std::memory_order_relaxed);
        x ^= static_cast<uint64_t>(getpid()) << 32;
        x ^= static_cast<uint64_t>(Clock::now().time_since_epoch().count());
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return static_cast<uint32_t>(x);
    }

    bool is_video_keyframe(const ProfileState &profile, const std::vector<NalUnit> &nals) const {
        for (const auto &nal : nals) {
            const uint8_t type = video_nal_type(profile, nal);
            if (profile.profile.video_codec == VideoCodec::H264) {
                if (type == 5) return true;
            } else if (type >= 16 && type <= 21) {
                return true;
            }
        }
        return false;
    }

    void add_video_nal(const ProfileState &profile, std::vector<RtpChunk> &chunks,
                       const uint8_t *data, size_t len) {
        const bool count = rtsp_debug_ && profile.profile.video_codec == VideoCodec::H265;
        const size_t before = chunks.size();
        const auto packetization = packetize_video_nal(
            profile.profile.video_codec, data, len, max_payload_, false,
            [&](const VideoRtpFragment &fragment) {
                chunks.push_back({fragment.data, fragment.size, fragment.prefix,
                                  static_cast<uint8_t>(fragment.prefix_size), false});
            });
        if (count) {
            switch (packetization) {
            case VideoPacketization::Invalid:
                h265_invalid_nals_.fetch_add(1, std::memory_order_relaxed);
                break;
            case VideoPacketization::Single:
                h265_single_nals_.fetch_add(1, std::memory_order_relaxed);
                break;
            case VideoPacketization::Fragmented:
                h265_fu_nals_.fetch_add(1, std::memory_order_relaxed);
                h265_fu_packets_.fetch_add(chunks.size() - before, std::memory_order_relaxed);
                break;
            }
        }
    }

    std::string sdp(int profile_index) {
        const ProfileState &profile = *profiles_[static_cast<size_t>(profile_index)];
        std::string s =
            "v=0\r\n"
            "o=- 0 0 IN IP4 0.0.0.0\r\n"
            "s=rk-hdmi-streamer\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "a=type:broadcast\r\n"
            "a=control:*\r\n"
            "a=range:npt=now-\r\n";
        if (profile.profile.video) {
            const auto parameters = video_parameter_sets(profile_index);
            s += "m=video 0 RTP/AVP 96\r\n";
            if (profile.profile.video_codec == VideoCodec::H264) {
                s += sdp_h264(parameters);
            } else {
                s += sdp_h265(parameters);
            }
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

    std::string sdp_h264(const ParameterSets &parameters) const {
        char profile_id[7];
        snprintf(profile_id, sizeof(profile_id), "%02x%02x%02x",
                 parameters.sps[1], parameters.sps[2], parameters.sps[3]);
        return
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=" + std::string(profile_id) +
            ";sprop-parameter-sets=" + base64_encode(parameters.sps.data(), parameters.sps.size()) + "," +
            base64_encode(parameters.pps.data(), parameters.pps.size()) + "\r\n"
            "a=control:trackID=0\r\n";
    }

    std::string sdp_h265(const ParameterSets &parameters) const {
        return
            "a=rtpmap:96 H265/90000\r\n"
            "a=fmtp:96 sprop-vps=" + base64_encode(parameters.vps.data(), parameters.vps.size()) +
            ";sprop-sps=" + base64_encode(parameters.sps.data(), parameters.sps.size()) +
            ";sprop-pps=" + base64_encode(parameters.pps.data(), parameters.pps.size()) + "\r\n"
            "a=control:trackID=0\r\n";
    }

    std::string content_base(const std::string &url) const {
        const size_t track = url.find("/trackID=");
        if (track != std::string::npos) return url.substr(0, track + 1);
        if (!url.empty() && url.back() == '/') return url;
        return url + "/";
    }

    static std::string path_from_rtsp_url(const std::string &url) {
        if (url == "*") return url;
        size_t path_pos = 0;
        const size_t scheme = url.find("://");
        if (scheme != std::string::npos) {
            path_pos = url.find('/', scheme + 3);
            if (path_pos == std::string::npos) return "/";
        }
        std::string path = url.substr(path_pos);
        const size_t query = path.find('?');
        if (query != std::string::npos) path.resize(query);
        if (path.empty()) return "/";
        return path;
    }

    int resolve_profile_index(const std::string &url) const {
        const std::string path = path_from_rtsp_url(url);
        for (size_t i = 0; i < profiles_.size(); ++i) {
            const std::string &profile_path = profiles_[i]->profile.path;
            if (path == profile_path) return static_cast<int>(i);
            if (path == profile_path + "/") return static_cast<int>(i);
            const std::string track_prefix = (profile_path == "/") ? "/trackID=" : profile_path + "/trackID=";
            if (path.rfind(track_prefix, 0) == 0) return static_cast<int>(i);
        }
        return -1;
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

    std::string profile_name(int profile_index) const {
        if (profile_index < 0 || profile_index >= static_cast<int>(profiles_.size())) return "";
        return profiles_[static_cast<size_t>(profile_index)]->profile.name;
    }

    static std::string audio_frame_ms_string(size_t audio_frame_frames) {
        if (audio_frame_frames == 120) return "2.5";
        return std::to_string(audio_frame_frames / 48);
    }

    std::string listen_addr_;
    std::vector<std::unique_ptr<ProfileState>> profiles_;
    mutable std::mutex profiles_mutex_;
    size_t max_payload_ = 1200;
    bool rtsp_debug_ = false;
    int max_clients_ = 3;
    int listen_fd_ = -1;
    int wake_fd_ = -1;
    int epoll_fd_ = -1;
    uint8_t listen_tag_ = 0;
    uint8_t wake_tag_ = 0;
    uint16_t port_ = 0;
    std::atomic_bool running_{false};
    std::thread io_thread_;

    // Owned exclusively by io_thread_.
    std::vector<std::unique_ptr<Session>> sessions_;
    uint64_t next_session_id_ = 1;

    FramePool pool_;

    // Shared between producer threads and io_thread_.
    mutable std::mutex active_mutex_;
    std::condition_variable active_cv_;
    int total_active_readers_ = 0;
    std::atomic_int active_readers_atomic_{0};
    uint32_t audio_timestamp_ = 0;
    bool wake_signaled_ = false;
    std::vector<InboxCmd> inbox_;

    std::atomic<uint64_t> h265_single_nals_{0};
    std::atomic<uint64_t> h265_fu_nals_{0};
    std::atomic<uint64_t> h265_fu_packets_{0};
    std::atomic<uint64_t> h265_invalid_nals_{0};
    std::atomic<uint32_t> random_counter_{0x13579bdfU};
    std::atomic<uint64_t> send_bytes_{0};
    std::atomic<uint64_t> rtp_packets_{0};
    std::atomic<uint64_t> queue_drops_{0};
    Clock::time_point stats_last_time_ = Clock::now();
    uint64_t stats_last_send_bytes_ = 0;
    uint64_t stats_last_rtp_packets_ = 0;
    uint64_t stats_last_queue_drops_ = 0;
    uint64_t stats_last_h265_single_nals_ = 0;
    uint64_t stats_last_h265_fu_nals_ = 0;
    uint64_t stats_last_h265_fu_packets_ = 0;
    uint64_t stats_last_h265_invalid_nals_ = 0;
};

RtspServer::RtspServer(const std::string &listen_addr, std::vector<StreamProfile> profiles,
                       int rtp_payload_size, bool rtsp_debug, int max_clients)
    : impl_(std::make_unique<Impl>(listen_addr, std::move(profiles),
                                   rtp_payload_size, rtsp_debug, max_clients)) {}

RtspServer::~RtspServer() = default;

uint16_t RtspServer::port() const { return impl_->port(); }
RtspServer::StreamSelection RtspServer::stream_selection() const { return impl_->stream_selection(); }
std::vector<RtspServer::StreamSelection> RtspServer::active_streams() const { return impl_->active_streams(); }
bool RtspServer::wait_for_clients(std::chrono::milliseconds timeout) { return impl_->wait_for_clients(timeout); }
bool RtspServer::consume_keyframe_request() { return impl_->consume_keyframe_request(); }
bool RtspServer::consume_keyframe_request(int profile_index) { return impl_->consume_keyframe_request(profile_index); }
void RtspServer::request_keyframe() { impl_->request_keyframe(); }
void RtspServer::request_keyframe(int profile_index) { impl_->request_keyframe(profile_index); }
void RtspServer::log_stats() { impl_->log_stats(); }
void RtspServer::invalidate_video(StreamSelection selection) { impl_->invalidate_video(selection); }
void RtspServer::set_video_header(StreamSelection selection, const std::vector<uint8_t> &header) {
    impl_->set_video_header(selection, header);
}
void RtspServer::write_video(const EncodedPacket &packet, StreamSelection selection) {
    impl_->write_video(packet, selection);
}
void RtspServer::write_audio_l16(const int16_t *samples, size_t frames, StreamSelection selection) {
    impl_->write_audio_l16(samples, frames, selection);
}
void RtspServer::write_audio_opus(const uint8_t *payload, size_t len, size_t frames,
                                  StreamSelection selection) {
    impl_->write_audio_opus(payload, len, frames, selection);
}
void RtspServer::write_audio_l16_all(const int16_t *samples, size_t frames) {
    impl_->write_audio_l16_all(samples, frames);
}
void RtspServer::write_audio_opus_all(const uint8_t *payload, size_t len, size_t frames) {
    impl_->write_audio_opus_all(payload, len, frames);
}
