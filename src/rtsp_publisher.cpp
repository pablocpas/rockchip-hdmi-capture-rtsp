#include "rtsp_publisher.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include "h264_utils.hpp"
#include "rtp_video.hpp"
#include "rtsp_address.hpp"
#include "rtsp_message.hpp"

namespace {

[[noreturn]] void die(const std::string &msg) {
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

struct RtspUrl {
    std::string host;
    std::string port;
    std::string full;
};

RtspUrl parse_rtsp_url(const std::string &url) {
    const auto addr = rtsp::publish_address(url);
    if (!addr) die("invalid RTSP publish URL");
    return {std::string(addr->host), std::string(addr->port), url};
}

class RtspPublisher : public MediaOutput {
public:
    RtspPublisher(const std::string &url,
                  const std::vector<uint8_t> &h264_header,
                  int fps,
                  bool audio,
                  const std::string &audio_codec,
                  int rtp_payload_size)
        : url_(parse_rtsp_url(url)),
          fps_(fps),
          audio_enabled_(audio),
          audio_codec_(audio_codec),
          max_payload_(static_cast<size_t>(rtp_payload_size)) {
        extract_sps_pps(h264_header);
        nal_scratch_.reserve(8);
        fragments_.reserve(64);
        try {
            connect_tcp();
            announce();
            setup_video();
            if (audio_enabled_) setup_audio();
            record();
        } catch (...) {
            if (fd_ >= 0) close(fd_);
            throw;
        }
    }

    ~RtspPublisher() {
        if (fd_ >= 0) close(fd_);
    }

    void write_packet(const EncodedPacket &packet) override {
        packet_nals(packet, nal_scratch_);
        if (nal_scratch_.empty()) return;

        fragments_.clear();
        for (const auto &nal : nal_scratch_) {
            if ((nal.data[0] & 0x1f) == 9) continue; // AUD is not useful here.
            packetize_video_nal(VideoCodec::H264, nal.data, nal.size, max_payload_, false,
                                [&](const VideoRtpFragment &fragment) {
                                    fragments_.push_back(fragment);
                                });
        }
        if (fragments_.empty()) return;
        fragments_.back().marker = true;

        const uint32_t ts = packet.has_rtp_timestamp ? packet.rtp_timestamp : timestamp_;
        if (packet.has_rtp_timestamp) {
            timestamp_ = packet.rtp_timestamp;
        } else {
            timestamp_ += 90000 / fps_;
        }

        std::lock_guard<std::mutex> lock(send_mutex_);
        for (size_t first = 0; first < fragments_.size();) {
            size_t count = 0;
            size_t bytes = 0;
            while (count < kMaxBatchPackets && first + count < fragments_.size()) {
                const auto &f = fragments_[first + count];
                const size_t wire = 16 + f.prefix_size + f.size;
                if (count > 0 && bytes + wire > kMaxBatchBytes) break;
                bytes += wire;
                ++count;
            }
            const bool more = first + count < fragments_.size();
            send_video_batch_unlocked(first, count, ts, more);
            first += count;
        }
    }

    void write_audio_l16_s16le(const int16_t *samples, size_t frames) override {
        if (!audio_enabled_ || frames == 0) return;

        constexpr size_t channels = 2;
        const size_t sample_count = frames * channels;
        audio_l16_buf_.resize(sample_count * sizeof(int16_t));
        uint8_t *dst = audio_l16_buf_.data();
        for (size_t i = 0; i < sample_count; ++i) {
            const uint16_t be = __builtin_bswap16(static_cast<uint16_t>(samples[i]));
            memcpy(dst + i * 2, &be, 2);
        }

        const bool marker = audio_marker_;
        audio_marker_ = false;
        send_rtp_packet(2, 97, audio_seq_++, audio_timestamp_, audio_ssrc_,
                        audio_l16_buf_.data(), audio_l16_buf_.size(), marker);
        audio_timestamp_ += static_cast<uint32_t>(frames);
    }

    void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) override {
        if (!audio_enabled_ || len == 0 || frames == 0) return;
        const bool marker = audio_marker_;
        audio_marker_ = false;
        send_rtp_packet(2, 97, audio_seq_++, audio_timestamp_, audio_ssrc_, payload, len, marker);
        audio_timestamp_ += static_cast<uint32_t>(frames);
    }

    void log_stats() override {
        std::lock_guard<std::mutex> lock(send_mutex_);
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - stats_last_time_).count();
        if (secs <= 0.0) return;

        uint64_t send_bytes = send_bytes_;
        uint64_t send_calls = send_calls_;
        uint64_t rtp_packets = rtp_packets_;
        uint64_t rtp_payload_bytes = rtp_payload_bytes_;
        uint64_t partial_sends = partial_sends_;

        uint64_t d_send_bytes = send_bytes - stats_last_send_bytes_;
        uint64_t d_send_calls = send_calls - stats_last_send_calls_;
        uint64_t d_rtp_packets = rtp_packets - stats_last_rtp_packets_;
        uint64_t d_rtp_payload_bytes = rtp_payload_bytes - stats_last_rtp_payload_bytes_;
        uint64_t d_partial_sends = partial_sends - stats_last_partial_sends_;

        double mbps = static_cast<double>(d_send_bytes) * 8.0 / secs / 1000000.0;
        double payload_mbps = static_cast<double>(d_rtp_payload_bytes) * 8.0 / secs / 1000000.0;
        double calls_per_packet = d_rtp_packets ? static_cast<double>(d_send_calls) / d_rtp_packets : 0.0;

        fprintf(stderr,
                "rtsp net Mbps=%.2f payload_Mbps=%.2f rtp_packets/s=%.0f send_calls/s=%.0f calls/packet=%.2f partial_sends=%llu\n",
                mbps,
                payload_mbps,
                static_cast<double>(d_rtp_packets) / secs,
                static_cast<double>(d_send_calls) / secs,
                calls_per_packet,
                static_cast<unsigned long long>(d_partial_sends));

        stats_last_time_ = now;
        stats_last_send_bytes_ = send_bytes;
        stats_last_send_calls_ = send_calls;
        stats_last_rtp_packets_ = rtp_packets;
        stats_last_rtp_payload_bytes_ = rtp_payload_bytes;
        stats_last_partial_sends_ = partial_sends;
    }

private:
    static constexpr size_t kMaxBatchPackets = 48;
    static constexpr size_t kMaxBatchBytes = 64 * 1024;

    void extract_sps_pps(const std::vector<uint8_t> &header) {
        for (const auto &nal : parse_annexb(header.data(), header.size())) {
            uint8_t type = nal.data[0] & 0x1f;
            if (type == 7) sps_.assign(nal.data, nal.data + nal.size);
            if (type == 8) pps_.assign(nal.data, nal.data + nal.size);
        }
        if (sps_.size() < 4 || pps_.empty()) die("H.264 header did not contain SPS/PPS");
    }

    void connect_tcp() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int ret = getaddrinfo(url_.host.c_str(), url_.port.c_str(), &hints, &res);
        if (ret) die(std::string("getaddrinfo failed: ") + gai_strerror(ret));

        for (addrinfo *rp = res; rp; rp = rp->ai_next) {
            fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd_ < 0) continue;
            if (connect(fd_, rp->ai_addr, rp->ai_addrlen) == 0) break;
            close(fd_);
            fd_ = -1;
        }
        freeaddrinfo(res);
        if (fd_ < 0) die("failed to connect to RTSP server");

        int yes = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        timeval timeout{5, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    void announce() {
        char profile[7];
        snprintf(profile, sizeof(profile), "%02x%02x%02x", sps_[1], sps_[2], sps_[3]);
        std::string sdp =
            "v=0\r\n"
            "o=- 0 0 IN IP4 127.0.0.1\r\n"
            "s=rk-hdmi-streamer\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "a=control:*\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=" + std::string(profile) +
            ";sprop-parameter-sets=" + base64_encode(sps_.data(), sps_.size()) + "," +
            base64_encode(pps_.data(), pps_.size()) + "\r\n"
            "a=control:trackID=0\r\n";

        if (audio_enabled_) {
            sdp += "m=audio 0 RTP/AVP 97\r\n";
            if (audio_codec_ == "opus") {
                sdp +=
                    "a=rtpmap:97 opus/48000/2\r\n"
                    "a=fmtp:97 stereo=1;sprop-stereo=1;useinbandfec=0\r\n"
                    "a=ptime:20\r\n"
                    "a=maxptime:20\r\n";
            } else {
                sdp += "a=rtpmap:97 L16/48000/2\r\n";
            }
            sdp += "a=control:trackID=1\r\n";
        }

        std::ostringstream req;
        req << "ANNOUNCE " << url_.full << " RTSP/1.0\r\n"
            << "CSeq: " << cseq_++ << "\r\n"
            << "Content-Type: application/sdp\r\n"
            << "Content-Length: " << sdp.size() << "\r\n\r\n"
            << sdp;
        send_request(req.str());
        read_response();
    }

    void setup_video() {
        std::ostringstream req;
        req << "SETUP " << url_.full << "/trackID=0 RTSP/1.0\r\n"
            << "CSeq: " << cseq_++ << "\r\n"
            << "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n";
        send_request(req.str());
        std::string resp = read_response();
        session_ = rtsp::session_token(rtsp::header_value(resp, "Session"));
        if (session_.empty()) die("RTSP SETUP response did not include Session header");
    }

    void setup_audio() {
        std::ostringstream req;
        req << "SETUP " << url_.full << "/trackID=1 RTSP/1.0\r\n"
            << "CSeq: " << cseq_++ << "\r\n"
            << "Session: " << session_ << "\r\n"
            << "Transport: RTP/AVP/TCP;unicast;interleaved=2-3;mode=record\r\n\r\n";
        send_request(req.str());
        read_response();
    }

    void record() {
        std::ostringstream req;
        req << "RECORD " << url_.full << " RTSP/1.0\r\n"
            << "CSeq: " << cseq_++ << "\r\n"
            << "Session: " << session_ << "\r\n"
            << "Range: npt=0.000-\r\n\r\n";
        send_request(req.str());
        read_response();
    }

    void send_request(const std::string &req) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_all_unlocked(reinterpret_cast<const uint8_t *>(req.data()), req.size());
    }

    std::string read_response() {
        for (;;) {
            const auto header_end = response_buffer_.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const auto header_size = header_end + 4;
                const auto body_size = rtsp::body_size(std::string_view(response_buffer_).substr(0, header_size));
                if (!body_size) die("invalid RTSP response length");
                const auto size = header_size + *body_size;
                if (response_buffer_.size() >= size) {
                    auto response = response_buffer_.substr(0, size);
                    response_buffer_.erase(0, size);
                    std::istringstream status{std::string(rtsp::first_line(response))};
                    std::string version;
                    int code = 0;
                    status >> version >> code;
                    if (version != "RTSP/1.0" || code != 200) die("RTSP request failed: " + status.str());
                    return response;
                }
            }
            char buf[1024];
            const auto count = recv(fd_, buf, sizeof(buf), 0);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) die(std::string("RTSP receive failed: ") + strerror(errno));
            if (count == 0) die("RTSP server closed connection");
            response_buffer_.append(buf, static_cast<size_t>(count));
            if (response_buffer_.size() > rtsp::max_message_size) die("RTSP response too large");
        }
    }

    void send_video_batch_unlocked(size_t first, size_t count, uint32_t timestamp, bool more) {
        std::array<std::array<uint8_t, 19>, kMaxBatchPackets> headers;
        std::array<iovec, 2 * kMaxBatchPackets> iov;
        uint64_t payload_bytes = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto &f = fragments_[first + i];
            const size_t payload_size = f.prefix_size + f.size;
            const size_t rtp_len = 12 + payload_size;
            if (rtp_len > 0xffff) die("RTP packet too large");
            auto &h = headers[i];
            h[0] = '$';
            h[1] = 0;
            write_be16(&h[2], static_cast<uint16_t>(rtp_len));
            h[4] = 0x80;
            h[5] = static_cast<uint8_t>((f.marker ? 0x80 : 0x00) | 96);
            write_be16(&h[6], seq_++);
            write_be32(&h[8], timestamp);
            write_be32(&h[12], ssrc_);
            h[16] = f.prefix[0];
            h[17] = f.prefix[1];
            h[18] = f.prefix[2];
            iov[2 * i] = {h.data(), 16 + f.prefix_size};
            iov[2 * i + 1] = {const_cast<uint8_t *>(f.data), f.size};
            payload_bytes += payload_size;
        }
        send_iov_all_unlocked(iov.data(), static_cast<int>(2 * count), more);
        rtp_packets_ += count;
        rtp_payload_bytes_ += payload_bytes;
    }

    void send_rtp_packet(uint8_t channel,
                         uint8_t payload_type,
                         uint16_t seq,
                         uint32_t timestamp,
                         uint32_t ssrc,
                         const uint8_t *payload,
                         size_t len,
                         bool marker) {
        const size_t rtp_len = 12 + len;
        if (rtp_len > 0xffff) die("RTP packet too large");
        uint8_t header[16];
        header[0] = '$';
        header[1] = channel;
        write_be16(&header[2], static_cast<uint16_t>(rtp_len));
        header[4] = 0x80;
        header[5] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | payload_type);
        write_be16(&header[6], seq);
        write_be32(&header[8], timestamp);
        write_be32(&header[12], ssrc);

        std::lock_guard<std::mutex> lock(send_mutex_);
        iovec iov[2] = {
            {header, sizeof(header)},
            {const_cast<uint8_t *>(payload), len},
        };
        send_iov_all_unlocked(iov, 2, false);
        ++rtp_packets_;
        rtp_payload_bytes_ += len;
    }

    void send_all_unlocked(const uint8_t *data, size_t len) {
        while (len) {
            ssize_t n = send(fd_, data, len, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) die(std::string("send failed: ") + strerror(errno));
            data += n;
            len -= static_cast<size_t>(n);
        }
    }

    void send_iov_all_unlocked(iovec *iov, int iovcnt, bool more) {
        const int flags = MSG_NOSIGNAL | (more ? MSG_MORE : 0);
        while (iovcnt > 0) {
            msghdr msg{};
            msg.msg_iov = iov;
            msg.msg_iovlen = static_cast<size_t>(iovcnt);

            ssize_t n = sendmsg(fd_, &msg, flags);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) die(std::string("sendmsg failed: ") + strerror(errno));
            ++send_calls_;
            send_bytes_ += static_cast<uint64_t>(n);

            ssize_t left = n;
            while (iovcnt > 0 && left >= static_cast<ssize_t>(iov[0].iov_len)) {
                left -= static_cast<ssize_t>(iov[0].iov_len);
                ++iov;
                --iovcnt;
            }
            if (iovcnt > 0 && left > 0) {
                iov[0].iov_base = static_cast<uint8_t *>(iov[0].iov_base) + left;
                iov[0].iov_len -= static_cast<size_t>(left);
                ++partial_sends_;
            }
        }
    }

    RtspUrl url_;
    int fps_ = 30;
    bool audio_enabled_ = false;
    std::string audio_codec_ = "l16";
    size_t max_payload_ = 1200;
    int fd_ = -1;
    int cseq_ = 1;
    std::string session_;
    std::string response_buffer_;
    uint16_t seq_ = 1;
    uint32_t timestamp_ = 0;
    uint32_t ssrc_ = 0x524b4831; // "RKH1"
    uint16_t audio_seq_ = 1;
    uint32_t audio_timestamp_ = 0;
    uint32_t audio_ssrc_ = 0x524b4131; // "RKA1"
    bool audio_marker_ = true;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    std::vector<NalUnit> nal_scratch_;
    std::vector<VideoRtpFragment> fragments_;
    std::vector<uint8_t> audio_l16_buf_;
    std::mutex send_mutex_;
    uint64_t rtp_packets_ = 0;
    uint64_t rtp_payload_bytes_ = 0;
    uint64_t send_calls_ = 0;
    uint64_t send_bytes_ = 0;
    uint64_t partial_sends_ = 0;
    std::chrono::steady_clock::time_point stats_last_time_ = std::chrono::steady_clock::now();
    uint64_t stats_last_rtp_packets_ = 0;
    uint64_t stats_last_rtp_payload_bytes_ = 0;
    uint64_t stats_last_send_calls_ = 0;
    uint64_t stats_last_send_bytes_ = 0;
    uint64_t stats_last_partial_sends_ = 0;
};

} // namespace

std::unique_ptr<MediaOutput> make_rtsp_publisher(const Options &opt, const std::vector<uint8_t> &header) {
    return std::make_unique<RtspPublisher>(opt.output, header, opt.fps, opt.audio, opt.audio_codec,
                                           opt.rtp_payload_size);
}
