#include "rtsp_server.hpp"
#include "rtsp_message.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int payload_limit = 256;
using Bytes = std::vector<uint8_t>;

void expect(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

struct InterleavedFrame {
    uint8_t channel;
    Bytes packet;
};

class Client {
public:
    explicit Client(uint16_t port, int receive_buffer = 0) : base_("rtsp://127.0.0.1:" + std::to_string(port)) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("could not create the RTSP test socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        timeval timeout{3, 0};
        if ((receive_buffer && setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) < 0) ||
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
            setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
            connect(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
            close(fd_);
            throw std::runtime_error("could not connect the RTSP test client");
        }
    }

    ~Client() { disconnect(); }

    void disconnect() {
        if (fd_ >= 0) close(std::exchange(fd_, -1));
    }

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    std::string request(const std::string &method, const std::string &path,
                        const std::string &headers = {}, int status = 200) {
        std::string sequence = std::to_string(++sequence_);
        std::string request = method + " " + base_ + path + " RTSP/1.0\r\nCSeq: " +
                              sequence + "\r\n" + headers + "\r\n";
        send_request(request);

        while (buffer_.empty() || buffer_.front() == '$') {
            if (buffer_.empty()) receive();
            if (buffer_.front() == '$') queued_frames_.push_back(read_interleaved());
        }

        size_t end = buffer_.find("\r\n\r\n");
        while (end == std::string::npos) {
            receive();
            end = buffer_.find("\r\n\r\n");
        }
        size_t header_size = end + 4;
        std::string response = buffer_.substr(0, header_size);
        expect(response.rfind("RTSP/1.0 " + std::to_string(status) + " ", 0) == 0,
               "unexpected RTSP response status");
        expect(header(response, "CSeq") == sequence, "response must preserve CSeq");
        std::string content_length = header(response, "Content-Length");
        size_t size = header_size + (content_length.empty() ? 0 : std::stoul(content_length));
        while (buffer_.size() < size) receive();
        response = buffer_.substr(0, size);
        buffer_.erase(0, size);
        return response;
    }

    void setup(const std::string &path, int track, int channel) {
        std::string transport = "Transport: RTP/AVP/TCP;unicast;interleaved=" +
                                std::to_string(channel) + "-" + std::to_string(channel + 1) + "\r\n";
        const auto response = request("SETUP", path + "/trackID=" + std::to_string(track),
                                      transport + session_header());
        std::string session = header(response, "Session");
        session_ = session.substr(0, session.find(';'));
        expect(!session_.empty(), "SETUP must return a session");
    }

    std::string session_request(const std::string &method, const std::string &path,
                                int status = 200) {
        return request(method, path, session_header(), status);
    }

    InterleavedFrame next_rtp() {
        for (;;) {
            InterleavedFrame frame;
            if (queued_frames_.empty()) {
                frame = read_interleaved();
            } else {
                frame = std::move(queued_frames_.front());
                queued_frames_.pop_front();
            }
            if (frame.channel % 2 == 0) {
                expect(frame.packet.size() >= 12 && frame.packet[0] == 0x80,
                       "expected an RTP packet with a fixed header");
                return frame;
            }
        }
    }

private:
    static std::string header(const std::string &response, const std::string &name) {
        std::string prefix = "\r\n" + name + ": ";
        size_t begin = response.find(prefix);
        if (begin == std::string::npos) return {};
        begin += prefix.size();
        return response.substr(begin, response.find("\r\n", begin) - begin);
    }

    std::string session_header() const {
        return session_.empty() ? std::string{} : "Session: " + session_ + "\r\n";
    }

    void send_request(const std::string &request) {
        size_t offset = 0;
        while (offset < request.size()) {
            auto sent = send(fd_, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) continue;
            expect(sent > 0, "could not send the RTSP request");
            offset += static_cast<size_t>(sent);
        }
    }

    void receive() {
        char data[4096];
        ssize_t size;
        do {
            size = recv(fd_, data, sizeof(data), 0);
        } while (size < 0 && errno == EINTR);
        expect(size > 0, "connection closed or timed out waiting for the next RTSP item");
        buffer_.append(data, static_cast<size_t>(size));
    }

    InterleavedFrame read_interleaved() {
        while (buffer_.size() < 4) receive();
        expect(buffer_.front() == '$', "expected an interleaved RTP or RTCP frame");
        uint8_t channel = static_cast<uint8_t>(buffer_[1]);
        size_t length = (static_cast<uint8_t>(buffer_[2]) << 8) |
                        static_cast<uint8_t>(buffer_[3]);
        while (buffer_.size() < 4 + length) receive();
        Bytes packet(buffer_.begin() + 4, buffer_.begin() + 4 + length);
        buffer_.erase(0, 4 + length);
        return {channel, std::move(packet)};
    }

    int fd_ = -1;
    int sequence_ = 0;
    std::string base_;
    std::string buffer_;
    std::string session_;
    std::deque<InterleavedFrame> queued_frames_;
};

RtspServer::StreamProfile profile(const std::string &name, VideoCodec codec, bool audio,
                                  bool video = true, int send_timeout_ms = 2000) {
    RtspServer::StreamProfile p{};
    p.name = name;
    p.path = "/" + name;
    p.video = video;
    p.video_codec = codec;
    if (video) {
        p.video_header_provider = [codec] {
            if (codec == VideoCodec::H264) {
                return Bytes{0, 0, 0, 1, 0x67, 0x64, 0x01, 0x20,
                             0, 0, 1, 0x68, 0x81};
            }
            return Bytes{0, 0, 0, 1, 0x40, 0x01, 0x80,
                         0, 0, 1, 0x42, 0x01, 0x81,
                         0, 0, 1, 0x44, 0x01, 0x82};
        };
    }
    p.fps = 25;
    p.audio = audio;
    p.audio_codec = "l16";
    p.audio_frame_frames = 960;
    p.send_timeout_ms = send_timeout_ms;
    return p;
}

void write_nal(RtspServer &server, RtspServer::StreamSelection selection, const Bytes &nal) {
    Bytes annex_b{0, 0, 0, 1};
    annex_b.insert(annex_b.end(), nal.begin(), nal.end());
    EncodedPacket packet{};
    packet.data = annex_b.data();
    packet.len = annex_b.size();
    server.write_video(packet, selection);
}

void expect_video_parameters(Client &client, std::initializer_list<uint8_t> headers,
                             uint8_t channel) {
    for (uint8_t header : headers) {
        auto frame = client.next_rtp();
        expect(frame.channel == channel && frame.packet[1] == 96 &&
               frame.packet.size() > 12 && frame.packet[12] == header,
               "video parameter sets must precede the first media frame");
    }
}

void expect_fragmented_nal(Client &client, const Bytes &nal, VideoCodec codec, uint8_t channel,
                           size_t max_payload = payload_limit) {
    const bool hevc = codec == VideoCodec::H265;
    const size_t header_size = hevc ? 2 : 1;
    const size_t fu_size = header_size + 1;
    const uint8_t nal_type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
    Bytes reassembled(nal.begin(), nal.begin() + header_size);
    Bytes timestamp;
    uint16_t previous_sequence = 0;
    const size_t fragment_payload = max_payload - fu_size;
    const size_t fragment_count = (nal.size() - header_size + fragment_payload - 1) / fragment_payload;
    for (size_t i = 0; i < fragment_count; ++i) {
        const bool last = i + 1 == fragment_count;
        auto frame = client.next_rtp();
        const Bytes &packet = frame.packet;
        expect(frame.channel == channel, "video used the wrong interleaved channel");
        expect(packet.size() > 12 + fu_size && packet.size() <= 12 + max_payload,
               "fragment size exceeded the negotiated payload limit");
        expect((packet[1] & 0x7f) == 96 && ((packet[1] & 0x80) != 0) == last,
               "RTP marker must appear on the final video fragment only");
        const uint8_t indicator = hevc ? (nal[0] & 0x81) | (49 << 1) : (nal[0] & 0xe0) | 28;
        expect(packet[12] == indicator, "wrong fragmentation-unit indicator");
        if (hevc) expect(packet[13] == nal[1], "HEVC layer and temporal ID must be preserved");
        expect(packet[12 + header_size] == (nal_type | (i == 0 ? 0x80 : last ? 0x40 : 0)),
               "incorrect fragmentation-unit start/end bits or NAL type");
        uint16_t sequence = static_cast<uint16_t>((packet[2] << 8) | packet[3]);
        Bytes current_timestamp(packet.begin() + 4, packet.begin() + 8);
        if (i == 0) {
            timestamp = current_timestamp;
        } else {
            expect(sequence == static_cast<uint16_t>(previous_sequence + 1),
                   "fragments must have consecutive RTP sequence numbers");
            expect(current_timestamp == timestamp, "fragments must share the same RTP timestamp");
        }
        previous_sequence = sequence;
        reassembled.insert(reassembled.end(), packet.begin() + 12 + fu_size, packet.end());
    }
    expect(reassembled == nal, "fragment reassembly must recover the original NAL");
}

void expect_small_nal(Client &client, const Bytes &nal, uint8_t channel) {
    auto frame = client.next_rtp();
    expect(frame.channel == channel && frame.packet[1] == (96 | 0x80),
           "expected a complete video NAL on the selected track");
    expect(Bytes(frame.packet.begin() + 12, frame.packet.end()) == nal,
           "received video other than the active producer's NAL");
}

void play(Client &client, RtspServer &server, const std::string &path) {
    const auto response = client.session_request("PLAY", path);
    expect(response.find("\r\nRTP-Info: ") != std::string::npos, "PLAY must advertise RTP-Info");
    client.session_request("GET_PARAMETER", path);
    expect(server.wait_for_clients(std::chrono::seconds(3)), "PLAY did not activate a reader");
}

void test_fragmentation_and_track_filtering() {
    RtspServer server("127.0.0.1:0", {profile("avc", VideoCodec::H264, true)},
                      payload_limit, false, 3);
    Client audio(server.port());
    Client video(server.port());
    const auto sdp = video.request("DESCRIBE", "/avc");
    expect(sdp.find("H264/90000") != std::string::npos, "DESCRIBE must advertise H.264");
    expect(sdp.find("L16/48000/2") != std::string::npos, "DESCRIBE must advertise stereo L16");
    audio.setup("/avc", 1, 6);
    video.setup("/avc", 0, 4);
    play(audio, server, "/avc");
    play(video, server, "/avc");
    const auto selection = server.stream_selection();
    const int16_t stereo[] = {0x1234, -2};
    server.write_audio_l16(stereo, 1, selection);
    Bytes nal(621, 0xad);
    nal[0] = 0x65;
    write_nal(server, selection, {0x41, 0xb0});
    write_nal(server, selection, nal);
    expect_video_parameters(video, {0x67, 0x68}, 4);
    expect_fragmented_nal(video, nal, VideoCodec::H264, 4);

    auto first = audio.next_rtp();
    expect(first.channel == 6 && first.packet[1] == (97 | 0x80),
           "first audio packet must use the requested channel and start marker");
    expect(Bytes(first.packet.begin() + 12, first.packet.end()) == Bytes{0x12, 0x34, 0xff, 0xfe},
           "L16 must preserve the signed samples in network byte order");

    server.write_audio_l16(stereo, 1, selection);
    auto second = audio.next_rtp();
    expect(second.channel == 6 && second.packet[1] == 97,
           "audio-only client received video or another start marker");
}

void test_profile_and_epoch_switches() {
    RtspServer server("127.0.0.1:0",
                      {profile("avc", VideoCodec::H264, true),
                       profile("hevc", VideoCodec::H265, false),
                       profile("audio", VideoCodec::H264, true, false, 8000)},
                      payload_limit, false, 6);
    Client avc(server.port());
    Client hevc(server.port());
    Client audio_only(server.port());

    const auto audio_sdp = audio_only.request("DESCRIBE", "/audio");
    expect(audio_sdp.find("m=video") == std::string::npos && audio_sdp.find("L16/48000/2") != std::string::npos,
           "audio-only DESCRIBE must omit video and advertise audio");
    audio_only.request("SETUP", "/audio/trackID=0",
                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 404);

    avc.setup("/avc", 0, 0);
    avc.setup("/avc", 1, 2);
    hevc.setup("/hevc", 0, 0);
    audio_only.setup("/audio", 1, 2);
    expect(hevc.request("DESCRIBE", "/hevc").find("H265/90000") != std::string::npos,
           "DESCRIBE must advertise H.265 on the HEVC stream");

    // All three streams (/avc, /hevc, /audio) must play simultaneously without 453.
    play(avc, server, "/avc");
    play(hevc, server, "/hevc");
    play(audio_only, server, "/audio");

    const auto streams = server.active_streams();
    expect(streams.size() == 3, "expected all three streams active simultaneously");
    const auto first_avc = streams[0];
    const auto active_hevc = streams[1];
    expect(first_avc.profile_index == 0 && active_hevc.profile_index == 1 && streams[2].profile_index == 2,
           "wrong active stream indices");
    expect(server.consume_keyframe_request(0) && server.consume_keyframe_request(1),
           "each active video stream should request its own initial keyframe");

    write_nal(server, first_avc, {0x65, 0xab});
    expect_video_parameters(avc, {0x67, 0x68}, 0);
    expect_small_nal(avc, {0x65, 0xab}, 0);

    Bytes hevc_nal(621, 0xad);
    hevc_nal[0] = 0x26;
    hevc_nal[1] = 0x01;
    write_nal(server, active_hevc, hevc_nal);
    expect_video_parameters(hevc, {0x40, 0x42, 0x44}, 0);
    expect_fragmented_nal(hevc, hevc_nal, VideoCodec::H265, 0);

    // Shared audio broadcast reaches both /avc and /audio with one call.
    const int16_t stereo[] = {0x1234, -2};
    server.write_audio_l16_all(stereo, 1);
    const auto avc_audio = avc.next_rtp();
    const auto only_audio = audio_only.next_rtp();
    expect(avc_audio.channel == 2 && avc_audio.packet[1] == (97 | 0x80),
           "/avc did not receive shared audio packet");
    expect(only_audio.channel == 2 && only_audio.packet[1] == (97 | 0x80) &&
               Bytes(only_audio.packet.begin() + 12, only_audio.packet.end()) == Bytes{0x12, 0x34, 0xff, 0xfe},
           "/audio did not receive shared audio packet");

    // Pausing /avc leaves /hevc and /audio active, and resuming /avc starts a new generation.
    avc.session_request("PAUSE", "/avc");
    expect(server.active_streams().size() == 2, "pausing /avc must keep /hevc and /audio active");

    play(avc, server, "/avc");
    const auto resumed = server.active_streams();
    expect(resumed.size() == 3 && resumed[0].profile_index == 0 && resumed[0].generation != first_avc.generation,
           "restarting a stream must advance its generation");
    write_nal(server, first_avc, {0x65, 0xba});
    const Bytes current_nal{0x65, 0xad};
    write_nal(server, resumed[0], current_nal);
    expect_video_parameters(avc, {0x67, 0x68}, 0);
    expect_small_nal(avc, current_nal, 0);

    avc.session_request("TEARDOWN", "/avc");
    hevc.session_request("TEARDOWN", "/hevc");
    audio_only.session_request("TEARDOWN", "/audio");
}

void test_video_recovers_on_the_same_session_after_capture_restarts() {
    RtspServer server("127.0.0.1:0", {profile("avc", VideoCodec::H264, false)},
                      payload_limit, false, 3);
    Client client(server.port());
    client.setup("/avc", 0, 0);
    play(client, server, "/avc");
    const auto selection = server.stream_selection();

    write_nal(server, selection, {0x65, 0xa1});
    expect_video_parameters(client, {0x67, 0x68}, 0);
    expect_small_nal(client, {0x65, 0xa1}, 0);
    write_nal(server, selection, {0x41, 0xa2});
    expect_small_nal(client, {0x41, 0xa2}, 0);

    server.invalidate_video(selection);
    write_nal(server, selection, {0x41, 0xb1});
    server.set_video_header(selection, {0, 0, 0, 1, 0x67, 0x64, 0x01, 0x2a,
                                        0, 0, 0, 1, 0x68, 0x82});
    write_nal(server, selection, {0x65, 0xb2});
    write_nal(server, selection, {0x41, 0xb3});

    for (const Bytes &nal : {Bytes{0x67, 0x64, 0x01, 0x2a}, Bytes{0x68, 0x82}}) {
        const auto parameter = client.next_rtp();
        expect(parameter.channel == 0 && parameter.packet[1] == 96 &&
               Bytes(parameter.packet.begin() + 12, parameter.packet.end()) == nal,
               "recovery must send the new encoder's parameter sets before any video");
    }
    expect_small_nal(client, {0x65, 0xb2}, 0);
    expect_small_nal(client, {0x41, 0xb3}, 0);
    client.session_request("TEARDOWN", "/avc");
}

void test_large_keyframe_can_restore_playback() {
    constexpr size_t max_payload = 60000;
    RtspServer server("127.0.0.1:0", {profile("avc", VideoCodec::H264, false)}, max_payload, false, 3);
    Client client(server.port());
    client.setup("/avc", 0, 0);
    play(client, server, "/avc");
    const auto selection = server.stream_selection();

    Bytes idr(900 * 1024, 0xaf);
    idr.front() = 0x65;
    {
        auto storage = std::make_shared<Bytes>(Bytes{0, 0, 0, 1});
        storage->insert(storage->end(), idr.begin(), idr.end());
        EncodedPacket packet{storage->data(), storage->size(), std::move(storage)};
        server.write_video(packet, selection);
    }
    write_nal(server, selection, {0x41, 0xb1});

    expect_video_parameters(client, {0x67, 0x68}, 0);
    expect_fragmented_nal(client, idr, VideoCodec::H264, 0, max_payload);
    expect_small_nal(client, {0x41, 0xb1}, 0);
}

void test_request_headers() {
    const std::string request =
        "SET_PARAMETER rtsp://127.0.0.1/avc RTSP/1.0\r\n"
        "cSeQ: 9\r\n"
        "content-Length: 3 \r\n\r\n"
        "yes";
    const auto header = std::string_view(request).substr(0, request.find("\r\n\r\n") + 4);
    expect(rtsp::header_value(request, "CSeq") == "9", "RTSP field names are case insensitive");
    expect(rtsp::body_size(header) == 3, "request parser must preserve the declared body size");
}

void test_slow_client_does_not_stall_other_viewers() {
    RtspServer server("127.0.0.1:0", {profile("avc", VideoCodec::H264, false)}, 60000, false, 3);
    Client slow(server.port(), 4096);
    Client fast(server.port());
    slow.setup("/avc", 0, 0);
    fast.setup("/avc", 0, 0);
    play(slow, server, "/avc");
    play(fast, server, "/avc");

    const auto selection = server.stream_selection();
    write_nal(server, selection, {0x65, 0xad});
    expect_video_parameters(fast, {0x67, 0x68}, 0);
    expect_small_nal(fast, {0x65, 0xad}, 0);

    Bytes nal(50000, 0xad);
    nal.front() = 0x41;
    for (unsigned i = 1; i <= 96; ++i) {
        nal.back() = static_cast<uint8_t>(i);
        write_nal(server, selection, nal);
        expect_small_nal(fast, nal, 0);
    }
}

size_t open_fds() {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) throw std::runtime_error("could not inspect open file descriptors");
    size_t count = 0;
    while (const auto *entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    closedir(directory);
    return count;
}

double seconds(const timeval &time) {
    return static_cast<double>(time.tv_sec) + time.tv_usec / 1e6;
}

rusage usage() {
    rusage result{};
    if (getrusage(RUSAGE_SELF, &result) < 0) throw std::runtime_error("getrusage failed");
    return result;
}

void stress(std::chrono::seconds duration) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    constexpr size_t max_payload = 1400;
    const auto fds = open_fds();
    const auto before = usage();
    const auto start = Clock::now();
    uint64_t frames = 0;
    unsigned rounds = 0;
    {
        RtspServer server("127.0.0.1:0",
            {profile("avc", VideoCodec::H264, true), profile("hevc", VideoCodec::H265, true)},
            max_payload, false, 8);

        while (Clock::now() - start < duration) {
            const bool hevc = rounds % 2 != 0;
            const auto codec = hevc ? VideoCodec::H265 : VideoCodec::H264;
            const std::string path = hevc ? "/hevc" : "/avc";
            Client slow(server.port(), 4096);
            Client first(server.port());
            Client second(server.port());
            slow.setup(path, 0, 0);
            first.setup(path, 0, 2);
            second.setup(path, 0, 4);
            play(slow, server, path);
            play(first, server, path);
            play(second, server, path);
            const auto selection = server.stream_selection();

            Bytes nal(40 * 1024, 0xa9);
            if (hevc) nal[1] = 1;
            auto next = Clock::now();
            for (unsigned frame = 0; frame < 100 && Clock::now() - start < duration; ++frame) {
                if (frame == 50) server.invalidate_video(selection);
                const bool keyframe = frame == 0 || frame == 50;
                nal[0] = hevc ? (keyframe ? 0x26 : 0x02) : (keyframe ? 0x65 : 0x41);
                nal.back() = static_cast<uint8_t>(frame + 1);
                write_nal(server, selection, nal);
                if (keyframe) {
                    if (hevc) {
                        expect_video_parameters(first, {0x40, 0x42, 0x44}, 2);
                        expect_video_parameters(second, {0x40, 0x42, 0x44}, 4);
                    } else {
                        expect_video_parameters(first, {0x67, 0x68}, 2);
                        expect_video_parameters(second, {0x67, 0x68}, 4);
                    }
                }
                expect_fragmented_nal(first, nal, codec, 2, max_payload);
                expect_fragmented_nal(second, nal, codec, 4, max_payload);
                ++frames;
                next += 20ms;
                std::this_thread::sleep_until(next);
            }

            // Close the slow TCP peer without a control exchange, as on a lost link.
            // The following round must be able to select a different profile.
            first.session_request("TEARDOWN", path);
            second.session_request("TEARDOWN", path);
            slow.disconnect();
            const auto timeout = Clock::now() + 3s;
            while (server.stream_selection() && Clock::now() < timeout) std::this_thread::sleep_for(1ms);
            expect(!server.stream_selection(), "readers remained active after clients disconnected");
            ++rounds;
        }
    }
    expect(open_fds() <= fds, "stress test leaked a file descriptor");
    const auto after = usage();
    const double wall_s = std::chrono::duration<double>(Clock::now() - start).count();
    const double cpu_s = seconds(after.ru_utime) + seconds(after.ru_stime) -
                         seconds(before.ru_utime) - seconds(before.ru_stime);
    std::cout << std::fixed << std::setprecision(2)
              << "frames=" << frames << " rounds=" << rounds << " wall_s=" << wall_s
              << " fps=" << (frames / wall_s) << " cpu_pct=" << (100 * cpu_s / wall_s)
              << " peak_rss_kib=" << after.ru_maxrss << '\n';
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 1) {
            if (argc != 3 || std::string_view(argv[1]) != "--stress") {
                throw std::runtime_error("usage: rtsp-test [--stress SECONDS]");
            }
            const std::string_view value(argv[2]);
            unsigned duration = 0;
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, duration);
            if (parsed.ec != std::errc{} || parsed.ptr != end || duration == 0 || duration > 86400) {
                throw std::runtime_error("stress duration must be between 1 and 86400 seconds");
            }
            stress(std::chrono::seconds(duration));
            return 0;
        }
        test_request_headers();
        test_fragmentation_and_track_filtering();
        test_profile_and_epoch_switches();
        test_video_recovers_on_the_same_session_after_capture_restarts();
        test_large_keyframe_can_restore_playback();
        test_slow_client_does_not_stall_other_viewers();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
