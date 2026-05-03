#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <alsa/asoundlib.h>
#ifdef HAVE_OPUS
#include <opus/opus.h>
#endif
#include <turbojpeg.h>

#include "rk_mpi.h"

namespace {

struct Options {
    std::string device = "/dev/video0";
    std::string audio_device = "hw:CARD=MS2109,DEV=0";
    std::string audio_codec = "l16";
    std::string output = "-";
    std::string decoder = "mppjpeg";
    std::string listen_rtsp;
    std::string rtsp_path = "/capture";
    double audio_gain = 1.0;
    bool audio = true;
    bool v4l2_dmabuf = false;
    int rtp_payload_size = 12000;
    int max_clients = 3;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrate = 8000000;
    int gop = 60;
    int frames = 0; // 0 means run forever.
};

bool starts_with(const std::string &s, const std::string &prefix) {
    return s.rfind(prefix, 0) == 0;
}

[[noreturn]] void die(const std::string &msg) {
    throw std::runtime_error(msg);
}

int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --device PATH      V4L2 device (default /dev/video0)\n"
            "  --audio-device ID  ALSA capture device (default hw:CARD=MS2109,DEV=0)\n"
            "  --audio-codec NAME  l16 or opus, if built with libopus (default l16)\n"
            "  --audio-gain N     PCM gain before encoding (default 1.0)\n"
            "  --rtp-payload N    Max RTP payload bytes (default 12000)\n"
            "  --no-audio         Disable RTSP audio track\n"
            "  --v4l2-dmabuf      Capture V4L2 MJPEG directly into MPP/DRM dma-buf buffers\n"
            "  --output PATH      H.264 Annex-B output path or - for stdout (default -)\n"
            "  --listen-rtsp ADDR Listen as RTSP/TCP server, e.g. :8554 or 0.0.0.0:8554\n"
            "  --rtsp-path PATH   RTSP server path (default /capture)\n"
            "  --max-clients N    Maximum RTSP server clients (default 3)\n"
            "  --decoder NAME     mppjpeg or turbojpeg (default mppjpeg)\n"
            "  --width N          Capture width (default 1920)\n"
            "  --height N         Capture height (default 1080)\n"
            "  --fps N            Capture/encode FPS (default 30)\n"
            "  --bitrate N        Target bitrate in bit/s (default 8000000)\n"
            "  --gop N            GOP length in frames (default 60)\n"
            "  --frames N         Stop after N frames (default 0 = forever)\n",
            argv0);
}

Options parse_args(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        auto need_value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                usage(argv[0]);
                die(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        std::string arg = argv[i];
        if (arg == "--device") opt.device = need_value("--device");
        else if (arg == "--audio-device") opt.audio_device = need_value("--audio-device");
        else if (arg == "--audio-codec") opt.audio_codec = need_value("--audio-codec");
        else if (arg == "--audio-gain") opt.audio_gain = atof(need_value("--audio-gain"));
        else if (arg == "--rtp-payload") opt.rtp_payload_size = atoi(need_value("--rtp-payload"));
        else if (arg == "--no-audio") opt.audio = false;
        else if (arg == "--v4l2-dmabuf") opt.v4l2_dmabuf = true;
        else if (arg == "--output") opt.output = need_value("--output");
        else if (arg == "--listen-rtsp") opt.listen_rtsp = need_value("--listen-rtsp");
        else if (arg == "--rtsp-path") opt.rtsp_path = need_value("--rtsp-path");
        else if (arg == "--max-clients") opt.max_clients = atoi(need_value("--max-clients"));
        else if (arg == "--decoder") opt.decoder = need_value("--decoder");
        else if (arg == "--width") opt.width = atoi(need_value("--width"));
        else if (arg == "--height") opt.height = atoi(need_value("--height"));
        else if (arg == "--fps") opt.fps = atoi(need_value("--fps"));
        else if (arg == "--bitrate") opt.bitrate = atoi(need_value("--bitrate"));
        else if (arg == "--gop") opt.gop = atoi(need_value("--gop"));
        else if (arg == "--frames") opt.frames = atoi(need_value("--frames"));
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            die("unknown argument: " + arg);
        }
    }

    if (opt.width <= 0 || opt.height <= 0 || opt.fps <= 0 || opt.bitrate <= 0 || opt.gop <= 0) {
        die("invalid numeric option");
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
    if (!opt.rtsp_path.empty() && opt.rtsp_path[0] != '/') {
        opt.rtsp_path.insert(opt.rtsp_path.begin(), '/');
    }
    if (opt.decoder != "mppjpeg" && opt.decoder != "turbojpeg") {
        die("--decoder must be mppjpeg or turbojpeg");
    }
    if (opt.audio_codec != "l16" && opt.audio_codec != "opus") {
        die("--audio-codec must be l16 or opus");
    }
#ifndef HAVE_OPUS
    if (opt.audio_codec == "opus") {
        die("opus audio requested but this binary was built without libopus");
    }
#endif
    return opt;
}

struct MmapBuffer {
    void *start = nullptr;
    size_t length = 0;
    int dmabuf_fd = -1;
    MppBuffer mpp_buffer = nullptr;
};

struct CapturedMjpegFrame {
    const uint8_t *data = nullptr;
    size_t bytesused = 0;
    size_t buffer_length = 0;
    int dmabuf_fd = -1;
    MppBuffer mpp_buffer = nullptr;
    unsigned index = 0;
};

class V4L2Capture {
public:
    explicit V4L2Capture(const Options &opt) : opt_(opt) {}

    ~V4L2Capture() {
        if (streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
        for (auto &b : buffers_) {
            if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
            if (!b.mpp_buffer && b.dmabuf_fd >= 0) close(b.dmabuf_fd);
        }
        if (fd_ >= 0) close(fd_);
    }

    void open_device() {
        fd_ = open(opt_.device.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0) die("failed to open " + opt_.device + ": " + strerror(errno));

        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP failed");
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) die("device is not video capture");
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) die("device does not support streaming");

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = opt_.width;
        fmt.fmt.pix.height = opt_.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT MJPEG failed");
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) die("device did not accept MJPEG format");
        if ((int)fmt.fmt.pix.width != opt_.width || (int)fmt.fmt.pix.height != opt_.height) {
            die("device changed requested resolution");
        }
        buffer_size_ = fmt.fmt.pix.sizeimage;

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = opt_.fps;
        if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) die("VIDIOC_S_PARM failed");
    }

    size_t buffer_size() const { return buffer_size_; }

    void start(const std::vector<MppBuffer> *capture_buffers = nullptr) {
        if (streaming_) return;
        const bool use_dmabuf = capture_buffers && !capture_buffers->empty();

        if (buffers_.empty()) {
            v4l2_requestbuffers req{};
            req.count = use_dmabuf ? static_cast<__u32>(capture_buffers->size()) : 6;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = use_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
            if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) die("VIDIOC_REQBUFS failed");
            if (req.count < 3) die("not enough V4L2 buffers");

            buffers_.resize(req.count);
            for (size_t i = 0; i < buffers_.size(); ++i) {
                if (use_dmabuf) {
                    MppBuffer mpp_buf = (*capture_buffers)[i];
                    buffers_[i].mpp_buffer = mpp_buf;
                    buffers_[i].length = mpp_buffer_get_size(mpp_buf);
                    buffers_[i].dmabuf_fd = mpp_buffer_get_fd(mpp_buf);
                    if (buffers_[i].dmabuf_fd < 0) die("MPP capture buffer has no dma-buf fd");
                } else {
                    v4l2_buffer buf{};
                    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buf.memory = V4L2_MEMORY_MMAP;
                    buf.index = i;
                    if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) die("VIDIOC_QUERYBUF failed");
                    buffers_[i].length = buf.length;
                    buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
                    if (buffers_[i].start == MAP_FAILED) die("mmap V4L2 buffer failed");
                }
            }

            if (use_dmabuf) {
                fprintf(stderr, "V4L2 DMABUF capture enabled into %zu MPP/DRM buffers\n",
                        buffers_.size());
            }
        } else if (use_dmabuf != (buffers_[0].mpp_buffer != nullptr)) {
            die("V4L2 capture memory mode changed after initialization");
        }

        for (size_t i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = use_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
            buf.index = i;
            if (use_dmabuf) {
                buf.length = buffers_[i].length;
                buf.m.fd = buffers_[i].dmabuf_fd;
            }
            if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) die("VIDIOC_QBUF failed");
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) die("VIDIOC_STREAMON failed");
        streaming_ = true;
    }

    void stop() {
        if (!streaming_) return;
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
            fprintf(stderr, "warning: VIDIOC_STREAMOFF failed: %s\n", strerror(errno));
        }
        streaming_ = false;
    }

    template <typename Fn>
    void read_frame(Fn fn) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 2000);
        if (pr < 0) die("poll failed");
        if (pr == 0) die("capture timeout");

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = buffers_[0].mpp_buffer ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return;
            die("VIDIOC_DQBUF failed");
        }

        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            ++dropped_error_buffers_;
            if (dropped_error_buffers_ <= 10 || dropped_error_buffers_ % 30 == 0) {
                fprintf(stderr, "dropped V4L2 error buffer count=%llu bytes=%u flags=0x%x\n",
                        static_cast<unsigned long long>(dropped_error_buffers_),
                        buf.bytesused,
                        buf.flags);
            }
        } else {
            const MmapBuffer &m = buffers_[buf.index];
            CapturedMjpegFrame frame{};
            frame.data = static_cast<const uint8_t *>(m.start);
            frame.bytesused = buf.bytesused;
            frame.buffer_length = m.length;
            frame.dmabuf_fd = m.dmabuf_fd;
            frame.mpp_buffer = m.mpp_buffer;
            frame.index = buf.index;
            fn(frame);
        }

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) die("VIDIOC_QBUF requeue failed");
    }

private:
    Options opt_;
    int fd_ = -1;
    bool streaming_ = false;
    size_t buffer_size_ = 0;
    uint64_t dropped_error_buffers_ = 0;
    std::vector<MmapBuffer> buffers_;
};

class TurboJpegDecoder {
public:
    TurboJpegDecoder(int width, int height) : width_(width), height_(height) {
        handle_ = tjInitDecompress();
        if (!handle_) die("tjInitDecompress failed");
        yuv420_.resize(width_ * height_ * 3 / 2);
        y422_.resize(width_ * height_);
        u422_.resize(width_ * height_ / 2);
        v422_.resize(width_ * height_ / 2);
    }

    ~TurboJpegDecoder() {
        if (handle_) tjDestroy(handle_);
    }

    const uint8_t *decode_to_i420(const uint8_t *jpeg, size_t jpeg_size) {
        if (jpeg_size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
            return nullptr;
        }

        int w = 0, h = 0, subsamp = 0, colorspace = 0;
        if (tjDecompressHeader3(handle_, jpeg, jpeg_size, &w, &h, &subsamp, &colorspace) < 0) {
            return nullptr;
        }
        if (w != width_ || h != height_) die("unexpected JPEG dimensions");

        uint8_t *planes[3] = { y422_.data(), u422_.data(), v422_.data() };
        int strides[3] = { width_, width_ / 2, width_ / 2 };
        if (tjDecompressToYUVPlanes(handle_, jpeg, jpeg_size, planes, width_, strides, height_, TJFLAG_FASTDCT) < 0) {
            return nullptr;
        }

        uint8_t *dst_y = yuv420_.data();
        uint8_t *dst_u = dst_y + width_ * height_;
        uint8_t *dst_v = dst_u + width_ * height_ / 4;

        memcpy(dst_y, y422_.data(), width_ * height_);

        if (subsamp == TJSAMP_420) {
            memcpy(dst_u, u422_.data(), width_ * height_ / 4);
            memcpy(dst_v, v422_.data(), width_ * height_ / 4);
        } else if (subsamp == TJSAMP_422) {
            // The MS2109 MJPEG stream is usually 4:2:2. Convert chroma to 4:2:0 by vertical averaging.
            for (int y = 0; y < height_ / 2; ++y) {
                const uint8_t *u0 = y422_plane_u(y * 2);
                const uint8_t *u1 = y422_plane_u(y * 2 + 1);
                const uint8_t *v0 = y422_plane_v(y * 2);
                const uint8_t *v1 = y422_plane_v(y * 2 + 1);
                uint8_t *du = dst_u + y * (width_ / 2);
                uint8_t *dv = dst_v + y * (width_ / 2);
                for (int x = 0; x < width_ / 2; ++x) {
                    du[x] = static_cast<uint8_t>((static_cast<int>(u0[x]) + u1[x] + 1) / 2);
                    dv[x] = static_cast<uint8_t>((static_cast<int>(v0[x]) + v1[x] + 1) / 2);
                }
            }
        } else {
            return nullptr;
        }

        return yuv420_.data();
    }

private:
    const uint8_t *y422_plane_u(int row) const { return u422_.data() + row * (width_ / 2); }
    const uint8_t *y422_plane_v(int row) const { return v422_.data() + row * (width_ / 2); }

    int width_;
    int height_;
    tjhandle handle_ = nullptr;
    std::vector<uint8_t> yuv420_;
    std::vector<uint8_t> y422_;
    std::vector<uint8_t> u422_;
    std::vector<uint8_t> v422_;
};

class MppJpegDecoder {
public:
    MppJpegDecoder(int width, int height) : width_(width), height_(height) {
        hor_stride_ = align16(width_);
        ver_stride_ = align16(height_);
        tight_i420_.resize(width_ * height_ * 3 / 2);
        init();
    }

    ~MppJpegDecoder() {
        if (frame_) mpp_frame_deinit(&frame_);
        for (auto &b : capture_packet_bufs_) {
            if (b) mpp_buffer_put(b);
        }
        if (frame_buf_) mpp_buffer_put(frame_buf_);
        if (packet_buf_) mpp_buffer_put(packet_buf_);
        if (buf_grp_) mpp_buffer_group_put(buf_grp_);
        if (cfg_) mpp_dec_cfg_deinit(cfg_);
        if (ctx_) mpp_destroy(ctx_);
    }

    int hor_stride() const { return hor_stride_; }
    int ver_stride() const { return ver_stride_; }
    MppFrameFormat format() const { return MPP_FMT_YUV420SP; }
    const std::vector<MppBuffer> &capture_buffers() const { return capture_packet_bufs_; }

    void init_capture_buffers(size_t buffer_size, size_t count) {
        if (!capture_packet_bufs_.empty()) return;
        if (!buffer_size || !count) die("invalid V4L2 DMABUF capture buffer configuration");
        capture_packet_bufs_.resize(count, nullptr);
        for (auto &buf : capture_packet_bufs_) {
            if (mpp_buffer_get(buf_grp_, &buf, buffer_size)) {
                die("mpp decoder V4L2 DMABUF packet buffer failed");
            }
        }
    }

    MppFrame decode_to_frame(const uint8_t *jpeg, size_t jpeg_size) {
        if (jpeg_size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
            return nullptr;
        }
        if (jpeg_size > packet_buf_size_) die("MJPEG frame is larger than packet buffer");

        memcpy(mpp_buffer_get_ptr(packet_buf_), jpeg, jpeg_size);
        ++copied_packets_;

        MppPacket packet = nullptr;
        if (mpp_packet_init_with_buffer(&packet, packet_buf_)) die("mpp_packet_init_with_buffer jpeg failed");
        mpp_packet_set_size(packet, jpeg_size);
        mpp_packet_set_length(packet, jpeg_size);

        return decode_packet(packet);
    }

    MppFrame decode_to_frame(const CapturedMjpegFrame &captured) {
        if (captured.mpp_buffer) {
            if (captured.bytesused < 4 || captured.bytesused > captured.buffer_length) {
                return nullptr;
            }

            MppPacket packet = nullptr;
            if (mpp_packet_init_with_buffer(&packet, captured.mpp_buffer)) {
                return nullptr;
            }
            mpp_packet_set_size(packet, captured.bytesused);
            mpp_packet_set_length(packet, captured.bytesused);
            ++dmabuf_packets_;

            MppFrame out = decode_packet(packet);
            if (out && !logged_dmabuf_active_) {
                fprintf(stderr, "mppjpeg input path: V4L2 captured directly into MPP/DRM dma-buf, no MJPEG packet memcpy\n");
                logged_dmabuf_active_ = true;
            }
            return out;
        }

        if (captured.bytesused < 4 || captured.data[0] != 0xff || captured.data[1] != 0xd8) {
            return nullptr;
        }
        return decode_to_frame(captured.data, captured.bytesused);
    }

    const uint8_t *decode_to_i420(const uint8_t *jpeg, size_t jpeg_size) {
        MppFrame out = decode_to_frame(jpeg, jpeg_size);
        if (!out) return nullptr;
        copy_frame_to_tight_i420(out);
        return tight_i420_.data();
    }

private:
    static int align16(int v) { return (v + 15) & ~15; }

    MppFrame decode_packet(MppPacket packet) {
        MppMeta meta = mpp_packet_get_meta(packet);
        if (meta) mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, frame_);

        MPP_RET ret = mpi_->decode_put_packet(ctx_, packet);
        if (ret) {
            mpp_packet_deinit(&packet);
            return nullptr;
        }

        MppFrame out = nullptr;
        ret = mpi_->decode_get_frame(ctx_, &out);
        mpp_packet_deinit(&packet);
        if (ret || !out) return nullptr;

        if (out != frame_) {
            mpp_frame_deinit(&out);
            return nullptr;
        }

        if (mpp_frame_get_errinfo(out) || mpp_frame_get_discard(out)) return nullptr;
        MppFrameFormat out_fmt = static_cast<MppFrameFormat>(mpp_frame_get_fmt(out) & MPP_FRAME_FMT_MASK);
        if (!logged_layout_) {
            fprintf(stderr, "mppjpeg output fmt=%d stride=%d:%d\n",
                    out_fmt, mpp_frame_get_hor_stride(out), mpp_frame_get_ver_stride(out));
            logged_layout_ = true;
        }
        if (out_fmt != MPP_FMT_YUV420SP) return nullptr;

        return out;
    }

    void init() {
        if (mpp_buffer_group_get_internal(&buf_grp_, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE)) {
            die("mpp decoder buffer group init failed");
        }

        packet_buf_size_ = static_cast<size_t>(width_) * height_ * 3;
        size_t frame_buf_size = static_cast<size_t>(hor_stride_) * ver_stride_ * 4;
        if (mpp_buffer_get(buf_grp_, &packet_buf_, packet_buf_size_)) die("mpp decoder packet buffer failed");
        if (mpp_buffer_get(buf_grp_, &frame_buf_, frame_buf_size)) die("mpp decoder frame buffer failed");

        if (mpp_frame_init(&frame_)) die("mpp decoder frame init failed");
        mpp_frame_set_width(frame_, width_);
        mpp_frame_set_height(frame_, height_);
        mpp_frame_set_hor_stride(frame_, hor_stride_);
        mpp_frame_set_ver_stride(frame_, ver_stride_);
        mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame_, frame_buf_);

        if (mpp_create(&ctx_, &mpi_)) die("mpp decoder create failed");
        if (mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG)) die("mpp jpeg decoder init failed");

        MppFrameFormat fmt = MPP_FMT_YUV420SP;
        mpi_->control(ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &fmt);

        if (mpp_dec_cfg_init(&cfg_)) die("mpp decoder cfg init failed");
        if (mpi_->control(ctx_, MPP_DEC_GET_CFG, cfg_)) die("mpp decoder get cfg failed");
        mpp_dec_cfg_set_u32(cfg_, "base:split_parse", 0);
        if (mpi_->control(ctx_, MPP_DEC_SET_CFG, cfg_)) die("mpp decoder set cfg failed");
    }

    void copy_frame_to_tight_i420(MppFrame frame) {
        MppFrameFormat fmt = mpp_frame_get_fmt(frame);
        int hs = mpp_frame_get_hor_stride(frame);
        int vs = mpp_frame_get_ver_stride(frame);
        auto *src = static_cast<uint8_t *>(mpp_buffer_get_ptr(mpp_frame_get_buffer(frame)));
        uint8_t *dy = tight_i420_.data();
        uint8_t *du = dy + width_ * height_;
        uint8_t *dv = du + width_ * height_ / 4;

        for (int y = 0; y < height_; ++y) {
            memcpy(dy + y * width_, src + y * hs, width_);
        }

        if ((fmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420P) {
            const uint8_t *su = src + hs * vs;
            const uint8_t *sv = su + (hs / 2) * (vs / 2);
            for (int y = 0; y < height_ / 2; ++y) {
                memcpy(du + y * (width_ / 2), su + y * (hs / 2), width_ / 2);
                memcpy(dv + y * (width_ / 2), sv + y * (hs / 2), width_ / 2);
            }
        } else if ((fmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP) {
            const uint8_t *suv = src + hs * vs;
            for (int y = 0; y < height_ / 2; ++y) {
                const uint8_t *row = suv + y * hs;
                uint8_t *urow = du + y * (width_ / 2);
                uint8_t *vrow = dv + y * (width_ / 2);
                for (int x = 0; x < width_ / 2; ++x) {
                    urow[x] = row[x * 2 + 0];
                    vrow[x] = row[x * 2 + 1];
                }
            }
        } else {
            die("unsupported MPP JPEG output format");
        }
    }

    int width_;
    int height_;
    int hor_stride_;
    int ver_stride_;
    size_t packet_buf_size_ = 0;
    std::vector<uint8_t> tight_i420_;
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppDecCfg cfg_ = nullptr;
    MppBufferGroup buf_grp_ = nullptr;
    MppBuffer packet_buf_ = nullptr;
    MppBuffer frame_buf_ = nullptr;
    MppFrame frame_ = nullptr;
    std::vector<MppBuffer> capture_packet_bufs_;
    uint64_t dmabuf_packets_ = 0;
    uint64_t copied_packets_ = 0;
    bool logged_dmabuf_active_ = false;
    bool logged_layout_ = false;
};

struct MppPacketDeleter {
    void operator()(MppPacket packet) const {
        if (packet) mpp_packet_deinit(&packet);
    }
};

using SharedMppPacket = std::shared_ptr<std::remove_pointer<MppPacket>::type>;

struct EncodedPacket {
    const uint8_t *data = nullptr;
    size_t len = 0;
    SharedMppPacket mpp_packet;
};

class MppH264Encoder {
public:
    explicit MppH264Encoder(const Options &opt,
                            int hor_stride,
                            int ver_stride,
                            MppFrameFormat fmt,
                            bool allocate_input_buffer)
        : opt_(opt),
          hor_stride_(hor_stride),
          ver_stride_(ver_stride),
          fmt_(fmt),
          allocate_input_buffer_(allocate_input_buffer) {
        frame_size_ = frame_size_for(fmt_, hor_stride_, ver_stride_);
        init();
    }

    ~MppH264Encoder() {
        if (frame_buf_) mpp_buffer_put(frame_buf_);
        if (packet_buf_) mpp_buffer_put(packet_buf_);
        if (buf_grp_) mpp_buffer_group_put(buf_grp_);
        if (cfg_) mpp_enc_cfg_deinit(cfg_);
        if (ctx_) mpp_destroy(ctx_);
    }

    template <typename Writer>
    void write_header(Writer writer) {
        MppPacket hdr = nullptr;
        mpp_packet_init_with_buffer(&hdr, packet_buf_);
        mpp_packet_set_length(hdr, 0);
        MPP_RET ret = mpi_->control(ctx_, MPP_ENC_GET_HDR_SYNC, hdr);
        if (ret) die("MPP_ENC_GET_HDR_SYNC failed");
        writer(static_cast<const uint8_t *>(mpp_packet_get_pos(hdr)), mpp_packet_get_length(hdr));
        mpp_packet_deinit(&hdr);
    }

    template <typename Writer>
    void encode_i420(const uint8_t *i420, Writer writer) {
        void *dst = mpp_buffer_get_ptr(frame_buf_);
        memcpy(dst, i420, frame_size_);

        MppFrame frame = nullptr;
        if (mpp_frame_init(&frame)) die("mpp_frame_init failed");
        mpp_frame_set_width(frame, opt_.width);
        mpp_frame_set_height(frame, opt_.height);
        mpp_frame_set_hor_stride(frame, hor_stride_);
        mpp_frame_set_ver_stride(frame, ver_stride_);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420P);
        mpp_frame_set_buffer(frame, frame_buf_);

        submit_frame(frame, writer);
    }

    template <typename Writer>
    void encode_mpp_frame(MppFrame decoded, Writer writer) {
        if ((int)mpp_frame_get_hor_stride(decoded) != hor_stride_ ||
            (int)mpp_frame_get_ver_stride(decoded) != ver_stride_ ||
            (mpp_frame_get_fmt(decoded) & MPP_FRAME_FMT_MASK) != (fmt_ & MPP_FRAME_FMT_MASK)) {
            die("decoded frame layout does not match encoder configuration");
        }

        MppFrame frame = nullptr;
        if (mpp_frame_init(&frame)) die("mpp_frame_init failed");
        mpp_frame_set_width(frame, opt_.width);
        mpp_frame_set_height(frame, opt_.height);
        mpp_frame_set_hor_stride(frame, hor_stride_);
        mpp_frame_set_ver_stride(frame, ver_stride_);
        mpp_frame_set_fmt(frame, fmt_);
        mpp_frame_set_buffer(frame, mpp_frame_get_buffer(decoded));

        submit_frame(frame, writer);
    }

private:
    static size_t frame_size_for(MppFrameFormat fmt, int hor_stride, int ver_stride) {
        switch (fmt & MPP_FRAME_FMT_MASK) {
            case MPP_FMT_YUV420P:
            case MPP_FMT_YUV420SP:
            case MPP_FMT_YUV420SP_VU:
                return static_cast<size_t>(hor_stride) * ver_stride * 3 / 2;
            default:
                die("unsupported encoder input format");
        }
    }

    template <typename Writer>
    void submit_frame(MppFrame frame, Writer writer) {
        MPP_RET ret = mpi_->encode_put_frame(ctx_, frame);
        mpp_frame_deinit(&frame);
        if (ret) die("encode_put_frame failed");

        MppPacket out = nullptr;
        ret = mpi_->encode_get_packet(ctx_, &out);
        if (ret) die("encode_get_packet failed");
        if (out) {
            SharedMppPacket owner(out, MppPacketDeleter{});
            EncodedPacket encoded{
                static_cast<const uint8_t *>(mpp_packet_get_pos(out)),
                mpp_packet_get_length(out),
                owner,
            };
            writer(encoded);
        }
    }

    void init() {
        MPP_RET ret = mpp_buffer_group_get_internal(&buf_grp_, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
        if (ret) die("mpp_buffer_group_get_internal failed");
        if (allocate_input_buffer_ && mpp_buffer_get(buf_grp_, &frame_buf_, frame_size_)) {
            die("mpp_buffer_get frame failed");
        }
        if (mpp_buffer_get(buf_grp_, &packet_buf_, frame_size_)) die("mpp_buffer_get packet failed");

        if (mpp_create(&ctx_, &mpi_)) die("mpp_create failed");

        MppPollType timeout = MPP_POLL_BLOCK;
        mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout);

        if (mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC)) die("mpp_init encoder failed");
        if (mpp_enc_cfg_init(&cfg_)) die("mpp_enc_cfg_init failed");
        if (mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_)) die("MPP_ENC_GET_CFG failed");

        mpp_enc_cfg_set_s32(cfg_, "codec:type", MPP_VIDEO_CodingAVC);
        mpp_enc_cfg_set_s32(cfg_, "prep:width", opt_.width);
        mpp_enc_cfg_set_s32(cfg_, "prep:height", opt_.height);
        mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", hor_stride_);
        mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", ver_stride_);
        mpp_enc_cfg_set_s32(cfg_, "prep:format", fmt_);
        mpp_enc_cfg_set_s32(cfg_, "prep:range", MPP_FRAME_RANGE_JPEG);

        mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", opt_.fps);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denom", 1);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", opt_.fps);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denom", 1);
        mpp_enc_cfg_set_s32(cfg_, "rc:gop", opt_.gop);
        mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", opt_.bitrate);
        mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", opt_.bitrate * 17 / 16);
        mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", opt_.bitrate * 15 / 16);
        mpp_enc_cfg_set_u32(cfg_, "rc:max_reenc_times", 0);
        mpp_enc_cfg_set_u32(cfg_, "rc:super_mode", 0);
        mpp_enc_cfg_set_u32(cfg_, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
        mpp_enc_cfg_set_u32(cfg_, "rc:drop_thd", 20);
        mpp_enc_cfg_set_u32(cfg_, "rc:drop_gap", 1);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_init", -1);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_max", 45);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_min", 10);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_max_i", 42);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_min_i", 10);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_ip", 2);

        mpp_enc_cfg_set_s32(cfg_, "h264:profile", 100);
        mpp_enc_cfg_set_s32(cfg_, "h264:level", opt_.height >= 1080 ? 40 : 31);
        mpp_enc_cfg_set_s32(cfg_, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(cfg_, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(cfg_, "h264:trans8x8", 1);

        if (mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_)) die("MPP_ENC_SET_CFG failed");

        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        if (mpi_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode)) {
            fprintf(stderr, "warning: MPP_ENC_SET_HEADER_MODE EACH_IDR failed\n");
        }

        MppEncSeiMode sei = MPP_ENC_SEI_MODE_DISABLE;
        if (mpi_->control(ctx_, MPP_ENC_SET_SEI_CFG, &sei)) {
            fprintf(stderr, "warning: MPP_ENC_SET_SEI_CFG DISABLE failed\n");
        }
    }

    Options opt_;
    int hor_stride_ = 0;
    int ver_stride_ = 0;
    MppFrameFormat fmt_ = MPP_FMT_YUV420P;
    bool allocate_input_buffer_ = true;
    size_t frame_size_ = 0;
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppEncCfg cfg_ = nullptr;
    MppBufferGroup buf_grp_ = nullptr;
    MppBuffer frame_buf_ = nullptr;
    MppBuffer packet_buf_ = nullptr;
};

class Output {
public:
    explicit Output(const std::string &path) {
        if (path == "-") {
            fp_ = stdout;
        } else {
            fp_ = fopen(path.c_str(), "wb");
            if (!fp_) die("failed to open output: " + path);
            own_ = true;
        }
    }

    ~Output() {
        if (fp_) fflush(fp_);
        if (own_ && fp_) fclose(fp_);
    }

    void write(const uint8_t *data, size_t len) {
        if (!len) return;
        if (fwrite(data, 1, len, fp_) != len) die("output write failed");
    }

private:
    FILE *fp_ = nullptr;
    bool own_ = false;
};

struct NalUnit {
    const uint8_t *data = nullptr;
    size_t size = 0;
};

bool strip_annexb_start_code(const uint8_t **data, size_t *size) {
    if (*size >= 4 && (*data)[0] == 0 && (*data)[1] == 0 && (*data)[2] == 0 && (*data)[3] == 1) {
        *data += 4;
        *size -= 4;
        return true;
    }
    if (*size >= 3 && (*data)[0] == 0 && (*data)[1] == 0 && (*data)[2] == 1) {
        *data += 3;
        *size -= 3;
        return true;
    }
    return false;
}

size_t find_start_code(const uint8_t *data, size_t size, size_t pos, size_t *prefix) {
    size_t i = pos;
    while (i + 3 <= size) {
        const void *next_zero = memchr(data + i, 0, size - i - 2);
        if (!next_zero) return size;
        i = static_cast<const uint8_t *>(next_zero) - data;

        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            *prefix = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *prefix = 3;
            return i;
        }
        ++i;
    }
    return size;
}

std::vector<NalUnit> parse_annexb(const uint8_t *data, size_t size) {
    std::vector<NalUnit> out;
    size_t prefix = 0;
    size_t start = find_start_code(data, size, 0, &prefix);
    while (start < size) {
        size_t nal_start = start + prefix;
        size_t next_prefix = 0;
        size_t next = find_start_code(data, size, nal_start, &next_prefix);
        size_t nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1] == 0) --nal_end;
        if (nal_end > nal_start) out.push_back({data + nal_start, nal_end - nal_start});
        start = next;
        prefix = next_prefix;
    }
    return out;
}

std::vector<NalUnit> nals_from_mpp_segments(const EncodedPacket &packet) {
    std::vector<NalUnit> out;
    if (!packet.mpp_packet || !packet.data || packet.len == 0) return out;

    MppPacket mpp_packet = packet.mpp_packet.get();
    RK_U32 segment_nb = mpp_packet_get_segment_nb(mpp_packet);
    const MppPktSeg *seg = mpp_packet_get_segment_info(mpp_packet);
    if (!segment_nb || !seg) return out;

    out.reserve(segment_nb);
    for (RK_U32 i = 0; i < segment_nb && seg; ++i, seg = seg->next) {
        if (seg->offset > packet.len || seg->len > packet.len - seg->offset) {
            out.clear();
            return out;
        }

        const uint8_t *nal = packet.data + seg->offset;
        size_t nal_size = seg->len;
        strip_annexb_start_code(&nal, &nal_size);
        while (nal_size > 0 && nal[nal_size - 1] == 0) --nal_size;
        if (nal_size > 0) out.push_back({nal, nal_size});
    }
    return out;
}

std::vector<NalUnit> packet_nals(const EncodedPacket &packet) {
    auto nals = nals_from_mpp_segments(packet);
    if (!nals.empty()) return nals;
    return parse_annexb(packet.data, packet.len);
}

std::string base64_encode(const uint8_t *data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(table[(v >> 18) & 0x3f]);
        out.push_back(table[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < len ? table[(v >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < len ? table[v & 0x3f] : '=');
    }
    return out;
}

struct RtspUrl {
    std::string host;
    std::string port = "554";
    std::string path;
    std::string full;
};

RtspUrl parse_rtsp_url(const std::string &url) {
    if (!starts_with(url, "rtsp://")) die("RTSP URL must start with rtsp://");
    RtspUrl u;
    u.full = url;
    std::string rest = url.substr(strlen("rtsp://"));
    size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == rest.size() - 1) die("RTSP URL must include a path");
    std::string hostport = rest.substr(0, slash);
    u.path = rest.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        u.host = hostport.substr(0, colon);
        u.port = hostport.substr(colon + 1);
    } else {
        u.host = hostport;
    }
    if (u.host.empty() || u.port.empty()) die("invalid RTSP URL host/port");
    return u;
}

class MediaOutput {
public:
    virtual ~MediaOutput() = default;
    virtual void write_frame(const uint8_t *data, size_t len) = 0;
    virtual void write_packet(const EncodedPacket &packet) { write_frame(packet.data, packet.len); }
    virtual void write_audio_l16_s16le(const int16_t *samples, size_t frames) = 0;
    virtual void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) = 0;
    virtual void log_stats() = 0;
};

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
        connect_tcp();
        announce();
        setup_video();
        if (audio_enabled_) setup_audio();
        record();
    }

    ~RtspPublisher() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    void write_frame(const uint8_t *data, size_t len) override {
        EncodedPacket packet{data, len, {}};
        write_packet(packet);
    }

    void write_packet(const EncodedPacket &packet) override {
        auto nals = packet_nals(packet);
        if (nals.empty()) return;

        size_t last_payload_nal = nals.size();
        for (size_t i = nals.size(); i > 0; --i) {
            uint8_t type = nals[i - 1].data[0] & 0x1f;
            if (type != 9) {
                last_payload_nal = i - 1;
                break;
            }
        }
        if (last_payload_nal == nals.size()) return;

        for (size_t i = 0; i < nals.size(); ++i) {
            uint8_t type = nals[i].data[0] & 0x1f;
            if (type == 9) continue; // AUD is not useful here.
            send_nal(nals[i].data, nals[i].size, i == last_payload_nal);
        }

        timestamp_ += 90000 / fps_;
    }

    void write_audio_l16_s16le(const int16_t *samples, size_t frames) override {
        if (!audio_enabled_ || frames == 0) return;

        constexpr size_t channels = 2;
        std::vector<uint8_t> payload(frames * channels * sizeof(int16_t));
        for (size_t i = 0; i < frames * channels; ++i) {
            uint16_t sample = static_cast<uint16_t>(samples[i]);
            payload[i * 2 + 0] = static_cast<uint8_t>(sample >> 8);
            payload[i * 2 + 1] = static_cast<uint8_t>(sample);
        }

        bool marker = audio_marker_;
        audio_marker_ = false;
        send_rtp_packet(2, 97, audio_seq_++, audio_timestamp_, audio_ssrc_, payload.data(), payload.size(), marker);
        audio_timestamp_ += frames;
    }

    void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) override {
        if (!audio_enabled_ || len == 0 || frames == 0) return;
        bool marker = audio_marker_;
        audio_marker_ = false;
        send_rtp_packet(2, 97, audio_seq_++, audio_timestamp_, audio_ssrc_, payload, len, marker);
        audio_timestamp_ += frames;
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
        read_response(true);
    }

    void setup_video() {
        std::ostringstream req;
        req << "SETUP " << url_.full << "/trackID=0 RTSP/1.0\r\n"
            << "CSeq: " << cseq_++ << "\r\n"
            << "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n";
        send_request(req.str());
        std::string resp = read_response(true);
        session_ = parse_session(resp);
        if (session_.empty()) die("RTSP SETUP response did not include Session header");
    }

    void setup_audio() {
        std::ostringstream req;
        req << "SETUP " << url_.full << "/trackID=1 RTSP/1.0\r\n"
            << "CSeq: " << cseq_++ << "\r\n"
            << "Session: " << session_ << "\r\n"
            << "Transport: RTP/AVP/TCP;unicast;interleaved=2-3;mode=record\r\n\r\n";
        send_request(req.str());
        read_response(true);
    }

    void record() {
        std::ostringstream req;
        req << "RECORD " << url_.full << " RTSP/1.0\r\n"
            << "CSeq: " << cseq_++ << "\r\n"
            << "Session: " << session_ << "\r\n"
            << "Range: npt=0.000-\r\n\r\n";
        send_request(req.str());
        read_response(true);
    }

    void send_request(const std::string &req) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_all_unlocked(reinterpret_cast<const uint8_t *>(req.data()), req.size());
    }

    std::string read_response(bool require_ok) {
        std::string resp;
        char buf[1024];
        while (resp.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) die("RTSP server closed connection");
            resp.append(buf, buf + n);
            if (resp.size() > 65536) die("RTSP response too large");
        }
        if (require_ok && resp.rfind("RTSP/1.0 200", 0) != 0) {
            die("RTSP request failed: " + resp.substr(0, resp.find("\r\n")));
        }
        return resp;
    }

    std::string parse_session(const std::string &resp) {
        std::string key = "\r\nSession:";
        size_t pos = resp.find(key);
        if (pos == std::string::npos) return {};
        pos += key.size();
        while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\t')) ++pos;
        size_t end = resp.find("\r\n", pos);
        std::string value = resp.substr(pos, end - pos);
        size_t semi = value.find(';');
        if (semi != std::string::npos) value.resize(semi);
        return value;
    }

    void send_nal(const uint8_t *nal, size_t len, bool marker) {
        size_t max_payload = max_payload_;
        if (len <= max_payload) {
            send_rtp(nal, len, marker);
            return;
        }

        uint8_t nal_header = nal[0];
        uint8_t fu_indicator = (nal_header & 0xe0) | 28;
        uint8_t nal_type = nal_header & 0x1f;
        size_t pos = 1;
        bool start = true;
        while (pos < len) {
            size_t chunk = std::min(max_payload - 2, len - pos);
            bool end = (pos + chunk) >= len;
            uint8_t fu_header[2] = {
                fu_indicator,
                static_cast<uint8_t>((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nal_type),
            };
            send_rtp_parts(fu_header, sizeof(fu_header), nal + pos, chunk, marker && end);
            pos += chunk;
            start = false;
        }
    }

    void send_rtp(const uint8_t *payload, size_t len, bool marker) {
        send_rtp_packet_parts(0, 96, seq_++, timestamp_, ssrc_, payload, len, nullptr, 0, marker);
    }

    void send_rtp_parts(const uint8_t *payload_a,
                        size_t len_a,
                        const uint8_t *payload_b,
                        size_t len_b,
                        bool marker) {
        send_rtp_packet_parts(0, 96, seq_++, timestamp_, ssrc_, payload_a, len_a, payload_b, len_b, marker);
    }

    void send_rtp_packet(uint8_t channel,
                         uint8_t payload_type,
                         uint16_t seq,
                         uint32_t timestamp,
                         uint32_t ssrc,
                         const uint8_t *payload,
                         size_t len,
                         bool marker) {
        send_rtp_packet_parts(channel, payload_type, seq, timestamp, ssrc, payload, len, nullptr, 0, marker);
    }

    void send_rtp_packet_parts(uint8_t channel,
                               uint8_t payload_type,
                               uint16_t seq,
                               uint32_t timestamp,
                               uint32_t ssrc,
                               const uint8_t *payload_a,
                               size_t len_a,
                               const uint8_t *payload_b,
                               size_t len_b,
                               bool marker) {
        size_t rtp_len = 12 + len_a + len_b;
        if (rtp_len > 0xffff) die("RTP packet too large");
        uint8_t header[4] = {
            '$',
            channel,
            static_cast<uint8_t>(rtp_len >> 8),
            static_cast<uint8_t>(rtp_len),
        };
        uint8_t rtp[12] = {
            0x80,
            static_cast<uint8_t>((marker ? 0x80 : 0x00) | payload_type),
            static_cast<uint8_t>(seq >> 8),
            static_cast<uint8_t>(seq),
            static_cast<uint8_t>(timestamp >> 24),
            static_cast<uint8_t>(timestamp >> 16),
            static_cast<uint8_t>(timestamp >> 8),
            static_cast<uint8_t>(timestamp),
            static_cast<uint8_t>(ssrc >> 24),
            static_cast<uint8_t>(ssrc >> 16),
            static_cast<uint8_t>(ssrc >> 8),
            static_cast<uint8_t>(ssrc),
        };

        std::lock_guard<std::mutex> lock(send_mutex_);
        iovec iov[4] = {
            {header, sizeof(header)},
            {rtp, sizeof(rtp)},
            {const_cast<uint8_t *>(payload_a), len_a},
            {const_cast<uint8_t *>(payload_b), payload_b ? len_b : 0},
        };
        int iovcnt = (payload_b && len_b) ? 4 : 3;
        send_iov_all_unlocked(iov, iovcnt);

        ++rtp_packets_;
        rtp_payload_bytes_ += len_a + len_b;
    }

    void send_all_unlocked(const uint8_t *data, size_t len) {
        while (len) {
            ssize_t n = send(fd_, data, len, MSG_NOSIGNAL);
            if (n < 0) die(std::string("send failed: ") + strerror(errno));
            data += n;
            len -= n;
        }
    }

    void send_iov_all_unlocked(iovec *iov, int iovcnt) {
        while (iovcnt > 0) {
            msghdr msg{};
            msg.msg_iov = iov;
            msg.msg_iovlen = static_cast<size_t>(iovcnt);

            ssize_t n = sendmsg(fd_, &msg, MSG_NOSIGNAL);
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
    uint16_t seq_ = 1;
    uint32_t timestamp_ = 0;
    uint32_t ssrc_ = 0x524b4831; // "RKH1"
    uint16_t audio_seq_ = 1;
    uint32_t audio_timestamp_ = 0;
    uint32_t audio_ssrc_ = 0x524b4131; // "RKA1"
    bool audio_marker_ = true;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
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

class RtspServer : public MediaOutput {
public:
    RtspServer(const std::string &listen_addr,
               const std::string &path,
               const std::vector<uint8_t> &h264_header,
               int fps,
               bool audio,
               const std::string &audio_codec,
               int rtp_payload_size,
               int max_clients)
        : listen_addr_(listen_addr),
          path_(path),
          fps_(fps),
          audio_enabled_(audio),
          audio_codec_(audio_codec),
          max_payload_(static_cast<size_t>(rtp_payload_size)),
          max_clients_(max_clients) {
        extract_sps_pps(h264_header);
        listen_tcp();
        running_.store(true, std::memory_order_relaxed);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    ~RtspServer() {
        running_.store(false, std::memory_order_relaxed);
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    bool has_clients() const {
        return active_readers_.load(std::memory_order_relaxed) > 0;
    }

    bool wait_for_clients(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(active_mutex_);
        active_cv_.wait_for(lock, timeout, [&] {
            return !running_.load(std::memory_order_relaxed) || has_clients();
        });
        return has_clients();
    }

    void write_frame(const uint8_t *data, size_t len) override {
        if (!has_clients()) return;

        EncodedPacket packet{data, len, {}};
        write_packet(packet);
    }

    void write_packet(const EncodedPacket &packet) override {
        if (!has_clients() || !packet.data || packet.len == 0) return;

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

        EncodedPacket storage_packet{storage.data, storage.size, storage.packet};
        auto nals = packet_nals(storage_packet);
        if (nals.empty()) return;

        size_t last_payload_nal = nals.size();
        for (size_t i = nals.size(); i > 0; --i) {
            uint8_t type = nals[i - 1].data[0] & 0x1f;
            if (type != 9) {
                last_payload_nal = i - 1;
                break;
            }
        }
        if (last_payload_nal == nals.size()) return;

        std::vector<RtpChunk> chunks;
        chunks.reserve(nals.size() + storage.size / max_payload_ + 1);
        for (size_t i = 0; i < nals.size(); ++i) {
            uint8_t type = nals[i].data[0] & 0x1f;
            if (type == 9) continue;
            size_t offset = static_cast<size_t>(nals[i].data - storage.data);
            add_video_nal(chunks, storage, offset, nals[i].size, i == last_payload_nal);
        }
        broadcast(chunks);
        video_timestamp_ += 90000 / fps_;
    }

    void write_audio_l16_s16le(const int16_t *samples, size_t frames) override {
        if (!audio_enabled_ || frames == 0 || !has_clients()) return;

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
        chunk.timestamp = audio_timestamp_;
        chunk.marker = audio_marker_;
        chunk.storage.bytes = payload;
        chunk.storage.data = payload->data();
        chunk.storage.size = payload->size();
        chunk.offset = 0;
        chunk.size = payload->size();
        audio_marker_ = false;
        audio_timestamp_ += frames;
        broadcast(std::vector<RtpChunk>{chunk});
    }

    void write_audio_payload(const uint8_t *payload, size_t len, size_t frames) override {
        if (!audio_enabled_ || len == 0 || frames == 0 || !has_clients()) return;
        auto data = std::make_shared<std::vector<uint8_t>>(payload, payload + len);
        RtpChunk chunk{};
        chunk.channel = 2;
        chunk.payload_type = 97;
        chunk.timestamp = audio_timestamp_;
        chunk.marker = audio_marker_;
        chunk.storage.bytes = data;
        chunk.storage.data = data->data();
        chunk.storage.size = data->size();
        chunk.offset = 0;
        chunk.size = data->size();
        audio_marker_ = false;
        audio_timestamp_ += frames;
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
        uint8_t prefix[2] = {};
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
        }

        ~Session() {
            close_socket();
        }

        void start() {
            auto self = shared_from_this();
            std::thread([self] { self->read_loop(); }).detach();
            std::thread([self] { self->write_loop(); }).detach();
        }

        bool closed() const { return closed_.load(std::memory_order_relaxed); }
        bool playing() const { return playing_.load(std::memory_order_relaxed); }
        uint64_t id() const { return id_; }

        void enqueue(const std::vector<RtpChunk> &chunks) {
            if (!playing() || closed()) return;
            std::lock_guard<std::mutex> lock(queue_mutex_);
            for (const auto &chunk : chunks) {
                size_t bytes = chunk.wire_size();
                while (!queue_.empty() && queue_bytes_ + bytes > max_queue_bytes_) {
                    queue_bytes_ -= queue_.front().wire_size();
                    queue_.pop_front();
                    ++dropped_;
                    server_.queue_drops_.fetch_add(1, std::memory_order_relaxed);
                }
                if (bytes > max_queue_bytes_) continue;
                queue_.push_back(chunk);
                queue_bytes_ += bytes;
            }
            queue_cv_.notify_one();
        }

    private:
        static constexpr size_t max_queue_bytes_ = 2 * 1024 * 1024;

        void read_loop() {
            std::string pending;
            while (!closed()) {
                std::string req;
                if (!read_request(pending, req)) break;
                if (!handle_request(req)) break;
            }
            bool was_playing = playing_.exchange(false, std::memory_order_relaxed);
            if (was_playing) server_.reader_stopped();
            closed_.store(true, std::memory_order_relaxed);
            close_socket();
            queue_cv_.notify_all();
            server_.remove_session(id_);
        }

        bool read_request(std::string &pending, std::string &req) {
            while (true) {
                size_t hdr_end = pending.find("\r\n\r\n");
                if (hdr_end != std::string::npos) {
                    req = pending.substr(0, hdr_end + 4);
                    pending.erase(0, hdr_end + 4);
                    return true;
                }
                char buf[2048];
                ssize_t n = recv(fd_, buf, sizeof(buf), 0);
                if (n <= 0) return false;
                pending.append(buf, buf + n);
                if (pending.size() > 65536) return false;
            }
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

        bool handle_request(const std::string &req) {
            std::string line = first_line(req);
            std::istringstream is(line);
            std::string method, url, version;
            is >> method >> url >> version;
            std::string cseq = header_value(req, "CSeq");
            if (cseq.empty()) cseq = "1";

            if (method == "OPTIONS") {
                return send_response(cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n");
            }
            if (method == "DESCRIBE") {
                std::string sdp = server_.sdp();
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n"
                     << "Content-Type: application/sdp\r\n"
                     << "Content-Base: " << server_.content_base(url) << "\r\n"
                     << "Content-Length: " << sdp.size() << "\r\n\r\n"
                     << sdp;
                return send_raw(resp.str());
            }
            if (method == "SETUP") {
                bool audio = url.find("trackID=1") != std::string::npos;
                if (audio && !server_.audio_enabled_) return send_error(cseq, 404, "Not Found");
                if (session_id_.empty()) session_id_ = std::to_string(id_);
                int rtp_ch = audio ? 2 : 0;
                int rtcp_ch = audio ? 3 : 1;
                std::ostringstream extra;
                extra << "Transport: RTP/AVP/TCP;unicast;interleaved=" << rtp_ch << "-" << rtcp_ch << "\r\n"
                      << "Session: " << session_id_ << "\r\n";
                return send_response(cseq, extra.str());
            }
            if (method == "PLAY") {
                if (!playing_.exchange(true, std::memory_order_relaxed)) server_.reader_started();
                std::ostringstream extra;
                extra << "Session: " << session_id_ << "\r\n"
                      << "RTP-Info: url=" << server_.content_base(url) << "trackID=0"
                      << ";seq=" << video_seq_ << ";rtptime=" << server_.video_timestamp() << "\r\n";
                return send_response(cseq, extra.str());
            }
            if (method == "PAUSE") {
                if (playing_.exchange(false, std::memory_order_relaxed)) server_.reader_stopped();
                return send_response(cseq, "Session: " + session_id_ + "\r\n");
            }
            if (method == "TEARDOWN") {
                send_response(cseq, "Session: " + session_id_ + "\r\n");
                return false;
            }
            return send_error(cseq, 405, "Method Not Allowed");
        }

        bool send_response(const std::string &cseq, const std::string &extra) {
            std::ostringstream resp;
            resp << "RTSP/1.0 200 OK\r\n"
                 << "CSeq: " << cseq << "\r\n"
                 << extra
                 << "\r\n";
            return send_raw(resp.str());
        }

        bool send_error(const std::string &cseq, int code, const std::string &text) {
            std::ostringstream resp;
            resp << "RTSP/1.0 " << code << " " << text << "\r\n"
                 << "CSeq: " << cseq << "\r\n\r\n";
            return send_raw(resp.str());
        }

        bool send_raw(const std::string &resp) {
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
                if (!send_rtp(chunk)) break;
            }
            closed_.store(true, std::memory_order_relaxed);
            close_socket();
        }

        bool send_rtp(const RtpChunk &chunk) {
            uint16_t &seq = (chunk.channel == 0) ? video_seq_ : audio_seq_;
            uint32_t ssrc = (chunk.channel == 0) ? video_ssrc_ : audio_ssrc_;
            size_t payload_size = chunk.payload_size();
            size_t rtp_len = 12 + payload_size;
            if (rtp_len > 0xffff) return false;
            uint8_t interleaved[4] = {
                '$',
                chunk.channel,
                static_cast<uint8_t>(rtp_len >> 8),
                static_cast<uint8_t>(rtp_len),
            };
            uint8_t rtp[12] = {
                0x80,
                static_cast<uint8_t>((chunk.marker ? 0x80 : 0x00) | chunk.payload_type),
                static_cast<uint8_t>(seq >> 8),
                static_cast<uint8_t>(seq),
                static_cast<uint8_t>(chunk.timestamp >> 24),
                static_cast<uint8_t>(chunk.timestamp >> 16),
                static_cast<uint8_t>(chunk.timestamp >> 8),
                static_cast<uint8_t>(chunk.timestamp),
                static_cast<uint8_t>(ssrc >> 24),
                static_cast<uint8_t>(ssrc >> 16),
                static_cast<uint8_t>(ssrc >> 8),
                static_cast<uint8_t>(ssrc),
            };
            ++seq;

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
            server_.rtp_packets_.fetch_add(1, std::memory_order_relaxed);
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
        std::string session_id_;
        uint16_t video_seq_ = 1;
        uint16_t audio_seq_ = 1;
        uint32_t video_ssrc_ = 0x524b5331; // "RKS1"
        uint32_t audio_ssrc_ = 0x524b5332; // "RKS2"
        std::mutex queue_mutex_;
        std::condition_variable queue_cv_;
        std::deque<RtpChunk> queue_;
        size_t queue_bytes_ = 0;
        uint64_t dropped_ = 0;
    };

    void extract_sps_pps(const std::vector<uint8_t> &header) {
        for (const auto &nal : parse_annexb(header.data(), header.size())) {
            uint8_t type = nal.data[0] & 0x1f;
            if (type == 7) sps_.assign(nal.data, nal.data + nal.size);
            if (type == 8) pps_.assign(nal.data, nal.data + nal.size);
        }
        if (sps_.size() < 4 || pps_.empty()) die("H.264 header did not contain SPS/PPS");
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
        if (ret) die(std::string("RTSP listen getaddrinfo failed: ") + gai_strerror(ret));

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
        if (listen_fd_ < 0) die("failed to listen for RTSP clients on " + listen_addr_);
        fprintf(stderr, "RTSP server listening on %s path=%s max_clients=%d\n",
                listen_addr_.c_str(), path_.c_str(), max_clients_);
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

    void reader_started() {
        active_readers_.fetch_add(1, std::memory_order_relaxed);
        active_cv_.notify_all();
    }

    void reader_stopped() {
        int prev = active_readers_.fetch_sub(1, std::memory_order_relaxed);
        if (prev <= 1) {
            active_readers_.store(0, std::memory_order_relaxed);
            active_cv_.notify_all();
        }
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

    void add_video_nal(std::vector<RtpChunk> &chunks,
                       const RtpStorage &storage,
                       size_t offset,
                       size_t len,
                       bool marker) {
        if (len <= max_payload_) {
            RtpChunk chunk{};
            chunk.channel = 0;
            chunk.payload_type = 96;
            chunk.timestamp = video_timestamp_;
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
            chunk.timestamp = video_timestamp_;
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

    std::string sdp() const {
        char profile[7];
        snprintf(profile, sizeof(profile), "%02x%02x%02x", sps_[1], sps_[2], sps_[3]);
        std::string s =
            "v=0\r\n"
            "o=- 0 0 IN IP4 0.0.0.0\r\n"
            "s=rk-hdmi-streamer\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=" + std::string(profile) +
            ";sprop-parameter-sets=" + base64_encode(sps_.data(), sps_.size()) + "," +
            base64_encode(pps_.data(), pps_.size()) + "\r\n"
            "a=control:trackID=0\r\n";
        if (audio_enabled_) {
            s += "m=audio 0 RTP/AVP 97\r\n";
            if (audio_codec_ == "opus") {
                s +=
                    "a=rtpmap:97 opus/48000/2\r\n"
                    "a=fmtp:97 stereo=1;sprop-stereo=1;useinbandfec=0\r\n"
                    "a=ptime:20\r\n"
                    "a=maxptime:20\r\n";
            } else {
                s += "a=rtpmap:97 L16/48000/2\r\n";
            }
            s += "a=control:trackID=1\r\n";
        }
        return s;
    }

    std::string content_base(const std::string &url) const {
        size_t track = url.find("/trackID=");
        if (track != std::string::npos) return url.substr(0, track + 1);
        if (!url.empty() && url.back() == '/') return url;
        return url + "/";
    }

    uint32_t video_timestamp() const { return video_timestamp_; }

    std::string listen_addr_;
    std::string path_;
    int fps_ = 30;
    bool audio_enabled_ = false;
    std::string audio_codec_;
    size_t max_payload_ = 1200;
    int max_clients_ = 3;
    int listen_fd_ = -1;
    std::atomic_bool running_{false};
    std::thread accept_thread_;
    std::mutex sessions_mutex_;
    std::vector<std::shared_ptr<Session>> sessions_;
    uint64_t next_session_id_ = 1;
    std::atomic_int active_readers_{0};
    std::mutex active_mutex_;
    std::condition_variable active_cv_;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    uint32_t video_timestamp_ = 0;
    uint32_t audio_timestamp_ = 0;
    bool audio_marker_ = true;
    std::atomic<uint64_t> send_bytes_{0};
    std::atomic<uint64_t> rtp_packets_{0};
    std::atomic<uint64_t> queue_drops_{0};
    std::chrono::steady_clock::time_point stats_last_time_ = std::chrono::steady_clock::now();
    uint64_t stats_last_send_bytes_ = 0;
    uint64_t stats_last_rtp_packets_ = 0;
    uint64_t stats_last_queue_drops_ = 0;
};

class AlsaAudioCapture {
public:
    AlsaAudioCapture(const std::string &device, const std::string &codec, double gain)
        : device_(device), codec_(codec), gain_(gain) {
        open_pcm();
        buffer_.resize(read_frames_ * channels_);
#ifdef HAVE_OPUS
        if (codec_ == "opus") {
            int err = 0;
            opus_ = opus_encoder_create(sample_rate_, channels_, OPUS_APPLICATION_AUDIO, &err);
            if (err != OPUS_OK || !opus_) die("Opus encoder init failed");
            opus_encoder_ctl(opus_, OPUS_SET_BITRATE(128000));
            opus_encoder_ctl(opus_, OPUS_SET_COMPLEXITY(4));
            opus_encoder_ctl(opus_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
            opus_encoder_ctl(opus_, OPUS_SET_INBAND_FEC(0));
            opus_encoder_ctl(opus_, OPUS_SET_PACKET_LOSS_PERC(0));
            opus_encoder_ctl(opus_, OPUS_SET_DTX(0));
            opus_encoder_ctl(opus_, OPUS_SET_LSB_DEPTH(16));
            opus_encoder_ctl(opus_, OPUS_SET_VBR(1));
            opus_encoder_ctl(opus_, OPUS_SET_VBR_CONSTRAINT(1));
            opus_packet_.resize(1500);
            pending_.reserve(opus_frames_ * channels_ * 2);
        }
#endif
    }

    ~AlsaAudioCapture() {
#ifdef HAVE_OPUS
        if (opus_) opus_encoder_destroy(opus_);
#endif
        if (pcm_) {
            snd_pcm_drop(pcm_);
            snd_pcm_close(pcm_);
        }
    }

    void run(std::atomic_bool &running, MediaOutput &publisher) {
        auto last_log = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_relaxed)) {
            snd_pcm_sframes_t frames = snd_pcm_readi(pcm_, buffer_.data(), read_frames_);
            if (frames == -EPIPE) {
                snd_pcm_prepare(pcm_);
                continue;
            }
            if (frames < 0) {
                frames = snd_pcm_recover(pcm_, frames, 1);
                if (frames < 0) {
                    fprintf(stderr, "ALSA read failed: %s\n", snd_strerror(frames));
                    reopen_after_error();
                    continue;
                }
            }
            if (frames > 0) {
                size_t got_frames = static_cast<size_t>(frames);
                audio_frames_ += got_frames;
                apply_gain(buffer_.data(), got_frames);
                update_peak(buffer_.data(), got_frames);
                write_audio(publisher, buffer_.data(), got_frames);
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 5) {
                    fprintf(stderr, "audio frames=%llu packets=%llu peak=%.3f codec=%s gain=%.2f\n",
                            static_cast<unsigned long long>(audio_frames_),
                            static_cast<unsigned long long>(audio_packets_),
                            static_cast<double>(audio_peak_) / 32768.0,
                            codec_.c_str(),
                            gain_);
                    audio_peak_ = 0;
                    last_log = now;
                }
            }
        }
    }

private:
    void open_pcm() {
        int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) die("ALSA open failed for " + device_ + ": " + snd_strerror(err));

        snd_pcm_hw_params_t *hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        if ((err = snd_pcm_hw_params_any(pcm_, hw)) < 0) die("ALSA hw params init failed");
        if ((err = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
            die("ALSA set access failed");
        }
        if ((err = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE)) < 0) {
            die("ALSA set format S16_LE failed");
        }
        if ((err = snd_pcm_hw_params_set_channels(pcm_, hw, channels_)) < 0) {
            die("ALSA set stereo failed");
        }
        unsigned rate = sample_rate_;
        if ((err = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr)) < 0 || rate != sample_rate_) {
            die("ALSA set 48000Hz failed");
        }
        snd_pcm_uframes_t period = read_frames_;
        snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr);
        snd_pcm_uframes_t buffer = read_frames_ * 8;
        snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer);
        if ((err = snd_pcm_hw_params(pcm_, hw)) < 0) die("ALSA apply hw params failed");

        if ((err = snd_pcm_prepare(pcm_)) < 0) die("ALSA prepare failed");
    }

    void reopen_after_error() {
        if (pcm_) {
            snd_pcm_drop(pcm_);
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        try {
            open_pcm();
        } catch (const std::exception &e) {
            fprintf(stderr, "ALSA reopen failed: %s\n", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void write_audio(MediaOutput &publisher, const int16_t *samples, size_t frames) {
        if (codec_ == "opus") {
            write_opus(publisher, samples, frames);
        } else {
            publisher.write_audio_l16_s16le(samples, frames);
            ++audio_packets_;
        }
    }

    void apply_gain(int16_t *samples, size_t frames) {
        if (gain_ == 1.0) return;
        for (size_t i = 0; i < frames * channels_; ++i) {
            int v = static_cast<int>(samples[i] * gain_);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            samples[i] = static_cast<int16_t>(v);
        }
    }

    void update_peak(const int16_t *samples, size_t frames) {
        for (size_t i = 0; i < frames * channels_; ++i) {
            int v = samples[i];
            if (v == -32768) v = 32767;
            if (v < 0) v = -v;
            if (v > audio_peak_) audio_peak_ = v;
        }
    }

    void write_opus(MediaOutput &publisher, const int16_t *samples, size_t frames) {
#ifdef HAVE_OPUS
        pending_.insert(pending_.end(), samples, samples + frames * channels_);
        const size_t needed_samples = opus_frames_ * channels_;
        while (pending_.size() >= needed_samples) {
            int bytes = opus_encode(opus_, pending_.data(), opus_frames_,
                                    opus_packet_.data(), static_cast<opus_int32>(opus_packet_.size()));
            if (bytes > 0) {
                publisher.write_audio_payload(opus_packet_.data(), static_cast<size_t>(bytes), opus_frames_);
                ++audio_packets_;
            } else {
                fprintf(stderr, "Opus encode failed: %s\n", opus_strerror(bytes));
            }
            pending_.erase(pending_.begin(), pending_.begin() + needed_samples);
        }
#else
        (void)publisher;
        (void)samples;
        (void)frames;
#endif
    }

    std::string device_;
    std::string codec_;
    double gain_ = 1.0;
    snd_pcm_t *pcm_ = nullptr;
    static constexpr unsigned sample_rate_ = 48000;
    static constexpr unsigned channels_ = 2;
    static constexpr snd_pcm_uframes_t read_frames_ = 480;
    static constexpr int opus_frames_ = 960;
    std::vector<int16_t> buffer_;
    std::vector<int16_t> pending_;
    uint64_t audio_frames_ = 0;
    uint64_t audio_packets_ = 0;
    int audio_peak_ = 0;
#ifdef HAVE_OPUS
    OpusEncoder *opus_ = nullptr;
    std::vector<uint8_t> opus_packet_;
#endif
};

class AudioRuntime {
public:
    ~AudioRuntime() {
        stop();
    }

    void start(AlsaAudioCapture &audio, MediaOutput &publisher) {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this, &audio, &publisher]() {
            try {
                audio.run(running_, publisher);
            } catch (const std::exception &e) {
                fprintf(stderr, "audio thread stopped: %s\n", e.what());
                running_.store(false, std::memory_order_relaxed);
            }
        });
    }

    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

private:
    std::atomic_bool running_{false};
    std::thread thread_;
};

} // namespace

int main(int argc, char **argv) {
    try {
        Options opt = parse_args(argc, argv);

        // Keep logs off stdout so stdout can be a clean H.264 bytestream.
        fprintf(stderr, "capture=%s %dx%d@%d MJPEG, decoder=%s, h264_mpp bitrate=%d output=%s\n",
                opt.device.c_str(), opt.width, opt.height, opt.fps, opt.decoder.c_str(),
                opt.bitrate, opt.output.c_str());

        std::unique_ptr<TurboJpegDecoder> turbojpeg;
        std::unique_ptr<MppJpegDecoder> mppjpeg;
        V4L2Capture cap(opt);
        cap.open_device();

        int enc_hor_stride = opt.width;
        int enc_ver_stride = opt.height;
        MppFrameFormat enc_fmt = MPP_FMT_YUV420P;
        bool enc_alloc_input = true;

        if (opt.decoder == "mppjpeg") {
            mppjpeg = std::make_unique<MppJpegDecoder>(opt.width, opt.height);
            if (opt.v4l2_dmabuf) {
                mppjpeg->init_capture_buffers(cap.buffer_size(), 6);
            }
            enc_hor_stride = mppjpeg->hor_stride();
            enc_ver_stride = mppjpeg->ver_stride();
            enc_fmt = mppjpeg->format();
            enc_alloc_input = false;
        } else {
            if (opt.v4l2_dmabuf) die("--v4l2-dmabuf requires --decoder mppjpeg");
            turbojpeg = std::make_unique<TurboJpegDecoder>(opt.width, opt.height);
        }

        MppH264Encoder enc(opt, enc_hor_stride, enc_ver_stride, enc_fmt, enc_alloc_input);

        std::vector<uint8_t> h264_header;
        enc.write_header([&](const uint8_t *data, size_t len) {
            h264_header.insert(h264_header.end(), data, data + len);
        });

        const bool serve_rtsp = !opt.listen_rtsp.empty();
        const bool publish_rtsp = !serve_rtsp && starts_with(opt.output, "rtsp://");
        std::unique_ptr<Output> out;
        std::unique_ptr<RtspPublisher> rtsp;
        std::unique_ptr<RtspServer> rtsp_server;
        MediaOutput *media_output = nullptr;
        std::unique_ptr<AlsaAudioCapture> audio;
        AudioRuntime audio_runtime;
        if (serve_rtsp) {
            rtsp_server = std::make_unique<RtspServer>(opt.listen_rtsp, opt.rtsp_path, h264_header,
                                                       opt.fps, opt.audio, opt.audio_codec,
                                                       opt.rtp_payload_size, opt.max_clients);
            media_output = rtsp_server.get();
        } else if (publish_rtsp) {
            rtsp = std::make_unique<RtspPublisher>(opt.output, h264_header, opt.fps, opt.audio,
                                                   opt.audio_codec, opt.rtp_payload_size);
            media_output = rtsp.get();
            if (opt.audio) {
                audio = std::make_unique<AlsaAudioCapture>(opt.audio_device, opt.audio_codec, opt.audio_gain);
                audio_runtime.start(*audio, *media_output);
                fprintf(stderr, "audio=%s PCM S16_LE 48000Hz stereo -> RTP %s gain=%.2f\n",
                        opt.audio_device.c_str(), opt.audio_codec.c_str(), opt.audio_gain);
            }
        } else {
            out = std::make_unique<Output>(opt.output);
            out->write(h264_header.data(), h264_header.size());
            if (opt.audio) {
                fprintf(stderr, "audio ignored for raw H.264 output; use --output rtsp://... to publish audio\n");
            }
        }

        int total_encoded = 0;
        int stats_encoded = 0;
        int stats_captured = 0;
        int stats_skipped = 0;
        auto stats_start = std::chrono::steady_clock::now();
        bool capture_started = false;
        auto start_capture = [&]() {
            if (capture_started) return;
            cap.start(opt.v4l2_dmabuf ? &mppjpeg->capture_buffers() : nullptr);
            capture_started = true;
            stats_encoded = 0;
            stats_captured = 0;
            stats_skipped = 0;
            stats_start = std::chrono::steady_clock::now();
            if (serve_rtsp && opt.audio) {
                audio = std::make_unique<AlsaAudioCapture>(opt.audio_device, opt.audio_codec, opt.audio_gain);
                audio_runtime.start(*audio, *media_output);
                fprintf(stderr, "audio=%s PCM S16_LE 48000Hz stereo -> RTP %s gain=%.2f\n",
                        opt.audio_device.c_str(), opt.audio_codec.c_str(), opt.audio_gain);
            }
        };

        auto stop_capture = [&]() {
            if (!capture_started) return;
            audio_runtime.stop();
            if (serve_rtsp) audio.reset();
            cap.stop();
            capture_started = false;
        };

        if (!serve_rtsp) start_capture();

        auto writer = [&](const EncodedPacket &packet) {
            if (serve_rtsp || publish_rtsp) media_output->write_packet(packet);
            else out->write(packet.data, packet.len);
        };

        while (opt.frames == 0 || total_encoded < opt.frames) {
            if (serve_rtsp && !rtsp_server->has_clients()) {
                stop_capture();
                rtsp_server->wait_for_clients(std::chrono::milliseconds(500));
                continue;
            }
            if (serve_rtsp) start_capture();

            cap.read_frame([&](const CapturedMjpegFrame &captured_frame) {
                ++stats_captured;
                if (opt.decoder == "mppjpeg") {
                    MppFrame frame = mppjpeg->decode_to_frame(captured_frame);
                    if (!frame) {
                        ++stats_skipped;
                        if (stats_skipped <= 10 || stats_skipped % opt.fps == 0) {
                            fprintf(stderr, "skipped corrupt/unsupported MJPEG frame captured=%d skipped=%d\n",
                                    stats_captured, stats_skipped);
                        }
                        return;
                    }
                    enc.encode_mpp_frame(frame, writer);
                } else {
                    const uint8_t *i420 = turbojpeg->decode_to_i420(captured_frame.data,
                                                                    captured_frame.bytesused);
                    if (!i420) {
                        ++stats_skipped;
                        if (stats_skipped <= 10 || stats_skipped % opt.fps == 0) {
                            fprintf(stderr, "skipped corrupt/unsupported MJPEG frame captured=%d skipped=%d\n",
                                    stats_captured, stats_skipped);
                        }
                        return;
                    }
                    enc.encode_i420(i420, writer);
                }
                ++total_encoded;
                ++stats_encoded;
                if (stats_encoded % opt.fps == 0) {
                    auto now = std::chrono::steady_clock::now();
                    double secs = std::chrono::duration<double>(now - stats_start).count();
                    fprintf(stderr, "captured=%d encoded=%d skipped=%d avg_encoded_fps=%.2f\n",
                            stats_captured, stats_encoded, stats_skipped,
                            stats_encoded / std::max(secs, 0.001));
                    if ((serve_rtsp || publish_rtsp) && stats_encoded % (opt.fps * 5) == 0) {
                        media_output->log_stats();
                    }
                }
            });
        }

        audio_runtime.stop();
        cap.stop();

        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
