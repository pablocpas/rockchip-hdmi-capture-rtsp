#include "stream_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>

#ifdef HAVE_LIBYUV
#include <libyuv/planar_functions.h>
#endif
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "capture_types.hpp"
#include "h264_utils.hpp"
#include "mpp_header.hpp"
#include "mpp_jpeg_packet.hpp"
#include "rga_converter.hpp"

namespace {

constexpr size_t kV4l2BufferCount = 4;
constexpr size_t kHeaderBufferSize = 4096;

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

void configure_mpp_timeouts(MppCtx ctx, MppApi *mpi) {
    constexpr int timeout_ms = 2000;
    MppPollType timeout = static_cast<MppPollType>(timeout_ms);
    if (mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &timeout) ||
        mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout)) {
        die("MPP rejected codec timeouts");
    }
}

MppFrameColorRange video_range_from_options(const Options &opt) {
    if (opt.video_range == "full") return MPP_FRAME_RANGE_JPEG;
    if (opt.video_range == "limited") return MPP_FRAME_RANGE_MPEG;
    return MPP_FRAME_RANGE_UNSPECIFIED;
}

struct MmapBuffer {
    void *start = nullptr;
    size_t length = 0;
    int dmabuf_fd = -1;
    MppBuffer mpp_buffer = nullptr;
};

uint64_t monotonic_time_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t timeval_to_ns(const timeval &tv) {
    if (tv.tv_sec == 0 && tv.tv_usec == 0) return monotonic_time_ns();
    return static_cast<uint64_t>(tv.tv_sec) * 1000000000ULL + static_cast<uint64_t>(tv.tv_usec) * 1000ULL;
}

const char *input_format_label(const std::string &format) {
    return format == "yuyv" ? "YUYV" : "MJPEG";
}

uint32_t v4l2_pixfmt_for_input(const std::string &input_format) {
    if (input_format == "mjpeg") return V4L2_PIX_FMT_MJPEG;
    if (input_format == "yuyv") return V4L2_PIX_FMT_YUYV;
    die("unsupported input format");
}

int align16(int value) {
    return (value + 15) & ~15;
}

void scale_nv12_cpu(const uint8_t *src, int src_w, int src_h, int src_hor_stride, int src_ver_stride,
                    uint8_t *dst, int dst_w, int dst_h, int dst_hor_stride, int dst_ver_stride) {
    const uint8_t *src_y = src;
    const uint8_t *src_uv = src + static_cast<size_t>(src_hor_stride) * src_ver_stride;
    uint8_t *dst_y = dst;
    uint8_t *dst_uv = dst + static_cast<size_t>(dst_hor_stride) * dst_ver_stride;

    for (int y = 0; y < dst_h; ++y) {
        const int sy = (y * src_h) / dst_h;
        const uint8_t *srow = src_y + static_cast<size_t>(sy) * src_hor_stride;
        uint8_t *drow = dst_y + static_cast<size_t>(y) * dst_hor_stride;
        for (int x = 0; x < dst_w; ++x) {
            drow[x] = srow[(x * src_w) / dst_w];
        }
    }
    const int src_ch = src_h / 2;
    const int dst_ch = dst_h / 2;
    const int src_cw = src_w / 2;
    const int dst_cw = dst_w / 2;
    for (int y = 0; y < dst_ch; ++y) {
        const int sy = (y * src_ch) / dst_ch;
        const uint8_t *srow = src_uv + static_cast<size_t>(sy) * src_hor_stride;
        uint8_t *drow = dst_uv + static_cast<size_t>(y) * dst_hor_stride;
        for (int x = 0; x < dst_cw; ++x) {
            const int sx = (x * src_cw) / dst_cw;
            drow[x * 2] = srow[sx * 2];
            drow[x * 2 + 1] = srow[sx * 2 + 1];
        }
    }
}

class V4L2Capture {
public:
    class FrameLease {
    public:
        FrameLease() = default;
        FrameLease(V4L2Capture &cap, const v4l2_buffer &buf, const CapturedFrame &frame)
            : cap_(&cap), buf_(buf), frame_(frame), armed_(true) {}

        ~FrameLease() {
            if (armed_) {
                if (xioctl(cap_->fd_, VIDIOC_QBUF, &buf_) < 0) {
                    fprintf(stderr, "warning: VIDIOC_QBUF requeue failed: %s\n", strerror(errno));
                }
            }
        }

        FrameLease(const FrameLease &) = delete;
        FrameLease &operator=(const FrameLease &) = delete;

        const CapturedFrame &frame() const { return frame_; }
        explicit operator bool() const { return armed_; }

        void requeue() {
            if (!armed_) return;
            armed_ = false;
            if (xioctl(cap_->fd_, VIDIOC_QBUF, &buf_) < 0) die("VIDIOC_QBUF requeue failed");
        }

    private:
        V4L2Capture *cap_ = nullptr;
        v4l2_buffer buf_{};
        CapturedFrame frame_{};
        bool armed_ = false;
    };

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
        fd_ = open(opt_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
        if (fd_ < 0) die("failed to open " + opt_.device + ": " + strerror(errno));

        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP failed");
        const auto capabilities = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE)) die("device is not video capture");
        if (!(capabilities & V4L2_CAP_STREAMING)) die("device does not support streaming");

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = opt_.width;
        fmt.fmt.pix.height = opt_.height;
        fmt.fmt.pix.pixelformat = v4l2_pixfmt_for_input(opt_.input_format);
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            die(std::string("VIDIOC_S_FMT ") + input_format_label(opt_.input_format) + " failed");
        }
        if (fmt.fmt.pix.pixelformat != v4l2_pixfmt_for_input(opt_.input_format)) {
            die(std::string("device did not accept ") + input_format_label(opt_.input_format) + " format");
        }
        if ((int)fmt.fmt.pix.width != opt_.width || (int)fmt.fmt.pix.height != opt_.height) {
            die("device changed requested resolution");
        }
        buffer_size_ = fmt.fmt.pix.sizeimage;
        bytesperline_ = fmt.fmt.pix.bytesperline;
        if (bytesperline_ == 0 && opt_.input_format == "yuyv") {
            bytesperline_ = static_cast<size_t>(opt_.width) * 2;
        }

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = opt_.fps;
        if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) die("VIDIOC_S_PARM failed");
        const auto &interval = parm.parm.capture.timeperframe;
        if (interval.numerator && interval.denominator &&
            static_cast<uint64_t>(interval.numerator) * opt_.fps != interval.denominator) {
            fprintf(stderr, "V4L2: requested %d fps, driver selected %u/%u fps\n",
                    opt_.fps, interval.denominator, interval.numerator);
        }
    }

    size_t buffer_size() const { return buffer_size_; }
    size_t bytesperline() const { return bytesperline_; }

    void start(const std::vector<MppBuffer> *capture_buffers = nullptr) {
        if (streaming_) return;
        const bool use_dmabuf = capture_buffers && !capture_buffers->empty();

        if (buffers_.empty()) {
            v4l2_requestbuffers req{};
            req.count = use_dmabuf ? static_cast<__u32>(capture_buffers->size())
                                   : static_cast<__u32>(kV4l2BufferCount);
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = use_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
            if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) die("VIDIOC_REQBUFS failed");
            if (req.count < 3) die("not enough V4L2 buffers");
            if (use_dmabuf && req.count > capture_buffers->size()) {
                die("driver requested more slots than the DMA-BUF pool provides");
            }

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
                    buf.index = static_cast<__u32>(i);
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
            buf.index = static_cast<__u32>(i);
            if (use_dmabuf) {
                buf.length = static_cast<__u32>(buffers_[i].length);
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

    std::optional<FrameLease> dequeue_frame(int wake_fd = -1) {
        pollfd pfds[2]{};
        pfds[0].fd = fd_;
        pfds[0].events = POLLIN;
        nfds_t nfds = 1;
        if (wake_fd >= 0) {
            pfds[1].fd = wake_fd;
            pfds[1].events = POLLIN;
            nfds = 2;
        }
        int pr = poll(pfds, nfds, 2000);
        if (pr < 0) {
            if (errno == EINTR) return std::nullopt;
            die(std::string("V4L2 poll failed: ") + strerror(errno));
        }
        if (pr == 0) die("capture timeout");
        if (wake_fd >= 0 && (pfds[1].revents & POLLIN)) return std::nullopt;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) die("V4L2 device unavailable");
        if (!(pfds[0].revents & POLLIN)) return std::nullopt;

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = buffers_[0].mpp_buffer ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return std::nullopt;
            die("VIDIOC_DQBUF failed");
        }

        if (buf.index >= buffers_.size()) die("V4L2 returned an invalid buffer index");
        const MmapBuffer &m = buffers_[buf.index];
        if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.bytesused > m.length) {
            ++dropped_error_buffers_;
            if (dropped_error_buffers_ <= 10 || dropped_error_buffers_ % 30 == 0) {
                fprintf(stderr, "dropped V4L2 error buffer count=%llu bytes=%u flags=0x%x\n",
                        static_cast<unsigned long long>(dropped_error_buffers_),
                        buf.bytesused,
                        buf.flags);
            }
            if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) die("VIDIOC_QBUF requeue failed");
            return std::nullopt;
        }

        CapturedFrame frame{};
        frame.data = static_cast<const uint8_t *>(m.start);
        frame.bytesused = buf.bytesused;
        frame.buffer_length = m.length;
        frame.bytesperline = bytesperline_;
        frame.timestamp_ns = timeval_to_ns(buf.timestamp);
        frame.dmabuf_fd = m.dmabuf_fd;
        frame.mpp_buffer = m.mpp_buffer;
        frame.index = buf.index;
        return std::optional<FrameLease>(std::in_place, *this, buf, frame);
    }

private:
    Options opt_;
    int fd_ = -1;
    bool streaming_ = false;
    size_t buffer_size_ = 0;
    size_t bytesperline_ = 0;
    uint64_t dropped_error_buffers_ = 0;
    std::vector<MmapBuffer> buffers_;
};

class MppDmabufPool {
public:
    MppDmabufPool(size_t buffer_size, size_t count, bool cachable = false) {
        if (!buffer_size || !count) die("invalid dma-buf pool configuration");
        const int type = cachable ? (MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE)
                                  : MPP_BUFFER_TYPE_DRM;
        if (mpp_buffer_group_get_internal(&grp_, type)) {
            die("dma-buf pool group init failed");
        }
        try {
            buffers_.resize(count, nullptr);
            for (auto &buf : buffers_) {
                if (mpp_buffer_get(grp_, &buf, buffer_size)) die("dma-buf pool allocation failed");
                if (mpp_buffer_get_fd(buf) < 0) die("allocated buffer has no dma-buf fd");
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~MppDmabufPool() { cleanup(); }

    MppDmabufPool(const MppDmabufPool &) = delete;
    MppDmabufPool &operator=(const MppDmabufPool &) = delete;

    const std::vector<MppBuffer> &buffers() const { return buffers_; }

private:
    void cleanup() {
        for (auto &buf : buffers_) {
            if (buf) mpp_buffer_put(buf);
            buf = nullptr;
        }
        buffers_.clear();
        if (grp_) mpp_buffer_group_put(grp_);
        grp_ = nullptr;
    }

    MppBufferGroup grp_ = nullptr;
    std::vector<MppBuffer> buffers_;
};

class MppJpegDecoder {
public:
    MppJpegDecoder(int width, int height, bool cpu_reads_output)
        : width_(width), height_(height), cpu_reads_output_(cpu_reads_output) {
        hor_stride_ = align16(width_);
        ver_stride_ = align16(height_);
        try {
            init();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~MppJpegDecoder() {
        cleanup();
    }

    void cleanup() {
        if (ctx_) mpp_destroy(ctx_);
        ctx_ = nullptr;
        for (auto &frame : frame_pool_) {
            if (frame) mpp_frame_deinit(&frame);
            frame = nullptr;
        }
        frame_pool_.clear();
        release_capture_buffers();
        for (auto &buf : frame_buf_pool_) {
            if (buf) mpp_buffer_put(buf);
            buf = nullptr;
        }
        frame_buf_pool_.clear();
        copy_packet_.reset();
        if (packet_buf_) mpp_buffer_put(packet_buf_);
        packet_buf_ = nullptr;
        if (copy_buf_grp_) mpp_buffer_group_put(copy_buf_grp_);
        copy_buf_grp_ = nullptr;
        if (buf_grp_) mpp_buffer_group_put(buf_grp_);
        buf_grp_ = nullptr;
        if (cfg_) mpp_dec_cfg_deinit(cfg_);
        cfg_ = nullptr;
    }

    const std::vector<MppBuffer> &capture_buffers() const {
        static const std::vector<MppBuffer> empty;
        return capture_pool_ ? capture_pool_->buffers() : empty;
    }

    void init_capture_buffers(size_t buffer_size, size_t count) {
        if (capture_pool_) return;
        capture_pool_ = std::make_unique<MppDmabufPool>(buffer_size, count, true);
        capture_packets_.reserve(count);
        for (MppBuffer buf : capture_pool_->buffers()) {
            capture_packets_.emplace_back(buf);
        }
    }

    void prepare_copy_buffer(size_t min_size = 0) {
        if (copy_packet_) return;
        packet_buf_size_ = std::max(min_size, static_cast<size_t>(width_) * height_ * 2);
        if (!copy_buf_grp_ &&
            mpp_buffer_group_get_internal(&copy_buf_grp_, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE)) {
            die("mpp decoder copy buffer group init failed");
        }
        if (!packet_buf_ && mpp_buffer_get(copy_buf_grp_, &packet_buf_, packet_buf_size_)) {
            die("mpp decoder packet buffer failed");
        }
        copy_packet_.emplace(packet_buf_);
    }

    void release_capture_buffers() {
        capture_packets_.clear();
        capture_pool_.reset();
    }

    static constexpr size_t frame_pool_size() { return kFramePoolSize; }

    MppFrame decode_to_frame(const uint8_t *jpeg, size_t jpeg_size,
                             std::optional<size_t> explicit_slot = std::nullopt) {
        if (jpeg_size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return nullptr;
        if (jpeg_size > packet_buf_size_) die("MJPEG frame is larger than packet buffer");

        memcpy(mpp_buffer_get_ptr(packet_buf_), jpeg, jpeg_size);
        MPP_RET sync_ret = mpp_buffer_sync_end(packet_buf_);
        if (sync_ret) {
            fprintf(stderr, "warning: mpp_buffer_sync_end jpeg packet failed ret=%d\n", sync_ret);
        }
        return decode_packet(copy_packet_->prepare(jpeg_size), explicit_slot);
    }

    MppFrame decode_to_frame(const CapturedFrame &captured,
                             std::optional<size_t> explicit_slot = std::nullopt) {
        if (captured.mpp_buffer) {
            if (captured.bytesused < 4 || captured.bytesused > captured.buffer_length) return nullptr;
            const auto index = captured.index;
            const auto &bufs = capture_buffers();
            if (index >= capture_packets_.size() || bufs[index] != captured.mpp_buffer) {
                die("V4L2 returned a different DMA-BUF than the decoder input slot");
            }
            MppFrame out = decode_packet(capture_packets_[index].prepare(captured.bytesused), explicit_slot);
            if (out && !logged_dmabuf_active_) {
                fprintf(stderr, "mppjpeg input path: V4L2 captured directly into MPP/DRM dma-buf, no MJPEG packet memcpy\n");
                logged_dmabuf_active_ = true;
            }
            return out;
        }
        return decode_to_frame(captured.data, captured.bytesused, explicit_slot);
    }

private:
    static constexpr size_t kFramePoolSize = 3;

    MppFrame decode_packet(MppPacket packet, std::optional<size_t> explicit_slot) {
        MppFrame target = explicit_slot ? output_frame_at(*explicit_slot) : next_output_frame();
        MppMeta meta = mpp_packet_get_meta(packet);
        if (!meta || mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, target)) {
            die("could not set MPP JPEG decoder output frame");
        }

        MPP_RET ret = mpi_->decode_put_packet(ctx_, packet);
        if (ret) die("MPP JPEG input failed: " + std::to_string(ret));

        MppFrame out = nullptr;
        ret = mpi_->decode_get_frame(ctx_, &out);
        if (ret == MPP_ERR_TIMEOUT) die("MPP JPEG decoder timed out");
        if (ret || !out) die("MPP JPEG output failed: " + std::to_string(ret));

        if (out != target) {
            if (std::find(frame_pool_.begin(), frame_pool_.end(), out) == frame_pool_.end()) {
                mpp_frame_deinit(&out);
            }
            die("MPP JPEG decoder returned an unexpected output frame");
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

    MppFrame output_frame_at(size_t slot) {
        if (slot >= frame_pool_.size()) die("MPP JPEG frame slot out of range");
        return frame_pool_[slot];
    }

    MppFrame next_output_frame() {
        if (frame_pool_.empty()) die("MPP JPEG frame pool is not initialized");
        MppFrame frame = frame_pool_[next_frame_];
        next_frame_ = (next_frame_ + 1) % frame_pool_.size();
        return frame;
    }

    void init() {
        const int group_type = cpu_reads_output_
            ? (MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE)
            : MPP_BUFFER_TYPE_DRM;
        if (mpp_buffer_group_get_internal(&buf_grp_, group_type)) {
            die("mpp decoder buffer group init failed");
        }

        const size_t frame_buf_size = static_cast<size_t>(hor_stride_) * ver_stride_ * 2;
        frame_buf_pool_.resize(kFramePoolSize, nullptr);
        frame_pool_.resize(kFramePoolSize, nullptr);
        for (size_t i = 0; i < kFramePoolSize; ++i) {
            if (mpp_buffer_get(buf_grp_, &frame_buf_pool_[i], frame_buf_size)) {
                die("mpp decoder frame buffer failed");
            }
            if (mpp_frame_init(&frame_pool_[i])) die("mpp decoder frame init failed");
            mpp_frame_set_width(frame_pool_[i], width_);
            mpp_frame_set_height(frame_pool_[i], height_);
            mpp_frame_set_hor_stride(frame_pool_[i], hor_stride_);
            mpp_frame_set_ver_stride(frame_pool_[i], ver_stride_);
            mpp_frame_set_fmt(frame_pool_[i], MPP_FMT_YUV420SP);
            mpp_frame_set_buffer(frame_pool_[i], frame_buf_pool_[i]);
        }
        fprintf(stderr, "mppjpeg output frame pool=%zu buffers (%zu KiB each) stride=%d:%d\n",
                frame_pool_.size(), frame_buf_size / 1024, hor_stride_, ver_stride_);

        if (mpp_create(&ctx_, &mpi_)) die("mpp decoder create failed");
        configure_mpp_timeouts(ctx_, mpi_);
        if (mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG)) die("mpp jpeg decoder init failed");

        MppFrameFormat fmt = MPP_FMT_YUV420SP;
        mpi_->control(ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &fmt);

        if (mpp_dec_cfg_init(&cfg_)) die("mpp decoder cfg init failed");
        if (mpi_->control(ctx_, MPP_DEC_GET_CFG, cfg_)) die("mpp decoder get cfg failed");
        mpp_dec_cfg_set_u32(cfg_, "base:split_parse", 0);
        if (mpi_->control(ctx_, MPP_DEC_SET_CFG, cfg_)) die("mpp decoder set cfg failed");
    }

    int width_;
    int height_;
    bool cpu_reads_output_ = false;
    int hor_stride_;
    int ver_stride_;
    size_t packet_buf_size_ = 0;
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppDecCfg cfg_ = nullptr;
    MppBufferGroup buf_grp_ = nullptr;
    MppBufferGroup copy_buf_grp_ = nullptr;
    MppBuffer packet_buf_ = nullptr;
    std::optional<MppJpegPacket> copy_packet_;
    std::vector<MppBuffer> frame_buf_pool_;
    std::vector<MppFrame> frame_pool_;
    size_t next_frame_ = 0;
    std::unique_ptr<MppDmabufPool> capture_pool_;
    std::vector<MppJpegPacket> capture_packets_;
    bool logged_dmabuf_active_ = false;
    bool logged_layout_ = false;
};

bool nals_from_mpp_segments(const EncodedPacket &packet, std::vector<NalUnit> &out) {
    out.clear();
    void *ctx = packet.nal_ctx ? packet.nal_ctx : packet.owner.get();
    if (!ctx || !packet.data || packet.len == 0) return false;

    MppPacket mpp_packet = static_cast<MppPacket>(ctx);
    RK_U32 segment_nb = mpp_packet_get_segment_nb(mpp_packet);
    const MppPktSeg *seg = mpp_packet_get_segment_info(mpp_packet);
    if (!segment_nb || !seg) return false;

    if (out.capacity() < segment_nb) out.reserve(segment_nb);
    for (RK_U32 i = 0; i < segment_nb && seg; ++i, seg = seg->next) {
        if (seg->offset > packet.len || seg->len > packet.len - seg->offset) {
            out.clear();
            return false;
        }
        const uint8_t *nal = packet.data + seg->offset;
        size_t nal_size = seg->len;
        strip_annexb_start_code(&nal, &nal_size);
        while (nal_size > 0 && nal[nal_size - 1] == 0) --nal_size;
        if (nal_size > 0) out.push_back({nal, nal_size});
    }
    return !out.empty();
}

void convert_yuyv_to_nv12_buffer(const uint8_t *yuyv, size_t bytes, size_t bytesperline,
                                 int width, int height, int hor_stride, int ver_stride,
                                 MppBuffer dst_buf) {
    const size_t row_size = static_cast<size_t>(width) * 2;
    const size_t src_stride = bytesperline ? bytesperline : row_size;
    if (src_stride < row_size || bytes < (height - 1) * src_stride + row_size) die("short YUYV frame");

    auto *dst = static_cast<uint8_t *>(mpp_buffer_get_ptr(dst_buf));
    uint8_t *dst_y = dst;
    uint8_t *dst_uv = dst_y + static_cast<size_t>(hor_stride) * ver_stride;

#ifdef HAVE_LIBYUV
    int ret = libyuv::YUY2ToNV12(yuyv,
                                 static_cast<int>(src_stride),
                                 dst_y,
                                 hor_stride,
                                 dst_uv,
                                 hor_stride,
                                 width,
                                 height);
    if (ret) die("libyuv YUY2ToNV12 failed");
#else
    for (int y = 0; y < height; y += 2) {
        const uint8_t *row0 = yuyv + static_cast<size_t>(y) * src_stride;
        const bool has_row1 = (y + 1 < height);
        const uint8_t *row1 = has_row1 ? row0 + src_stride : row0;
        uint8_t *dy0 = dst_y + static_cast<size_t>(y) * hor_stride;
        uint8_t *dy1 = has_row1 ? dy0 + hor_stride : nullptr;
        uint8_t *duv = dst_uv + static_cast<size_t>(y / 2) * hor_stride;
        int x = 0;
#ifdef __ARM_NEON
        for (; x + 32 <= width; x += 32) {
            const uint8x16x4_t r0 = vld4q_u8(row0 + static_cast<size_t>(x) * 2);
            const uint8x16x4_t r1 = vld4q_u8(row1 + static_cast<size_t>(x) * 2);
            const uint8x16x2_t y0 = {{r0.val[0], r0.val[2]}};
            vst2q_u8(dy0 + x, y0);
            if (dy1) {
                const uint8x16x2_t y1 = {{r1.val[0], r1.val[2]}};
                vst2q_u8(dy1 + x, y1);
            }
            const uint8x16x2_t uv = {{vrhaddq_u8(r0.val[1], r1.val[1]),
                                      vrhaddq_u8(r0.val[3], r1.val[3])}};
            vst2q_u8(duv + x, uv);
        }
#endif
        for (; x < width; x += 2) {
            const uint8_t *p0 = row0 + static_cast<size_t>(x) * 2;
            const uint8_t *p1 = row1 + static_cast<size_t>(x) * 2;
            dy0[x] = p0[0];
            dy0[x + 1] = p0[2];
            if (dy1) {
                dy1[x] = p1[0];
                dy1[x + 1] = p1[2];
            }
            duv[x] = static_cast<uint8_t>((static_cast<unsigned>(p0[1]) + p1[1] + 1) >> 1);
            duv[x + 1] = static_cast<uint8_t>((static_cast<unsigned>(p0[3]) + p1[3] + 1) >> 1);
        }
    }
#endif
    MPP_RET sync_ret = mpp_buffer_sync_end(dst_buf);
    if (sync_ret) {
        fprintf(stderr, "warning: mpp_buffer_sync_end capture nv12 failed ret=%d\n", sync_ret);
    }
}

class MppVideoEncoder {
public:
    MppVideoEncoder(const Options &opt, bool allocate_input_buffer, bool cpu_writes_input = true)
        : opt_(opt),
          hor_stride_(align16(opt.width)),
          ver_stride_(align16(opt.height)),
          allocate_input_buffer_(allocate_input_buffer),
          cpu_writes_input_(cpu_writes_input),
          frame_size_(static_cast<size_t>(hor_stride_) * ver_stride_ * 3 / 2) {
        try {
            init();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~MppVideoEncoder() { cleanup(); }

    int hor_stride() const { return hor_stride_; }
    int ver_stride() const { return ver_stride_; }
    int width() const { return opt_.width; }
    int height() const { return opt_.height; }
    MppBuffer input_buffer() const { return frame_buf_; }

    int input_buffer_fd() const {
        if (!frame_buf_) return -1;
        return mpp_buffer_get_fd(frame_buf_);
    }

    size_t input_buffer_size() const { return frame_size_; }

    std::vector<uint8_t> read_header() {
        return read_mpp_video_header(ctx_, *mpi_, kHeaderBufferSize);
    }

    template <typename Writer>
    void submit_nv12_frame(Writer writer) {
        submit_buffer(frame_buf_, writer);
    }

    template <typename Writer>
    void encode_nv12_buffer(MppBuffer src_buffer, int src_hor_stride, int src_ver_stride, Writer writer) {
        if (src_hor_stride != hor_stride_ || src_ver_stride != ver_stride_) {
            die("NV12 frame stride does not match encoder configuration");
        }
        submit_buffer(src_buffer, writer);
    }

    template <typename Writer>
    void encode_nv12_buffer_copy(MppBuffer src_buffer, int src_hor_stride, int src_ver_stride, Writer writer) {
        if (!frame_buf_) die("encoder copy mode requires an encoder input buffer");
        MPP_RET sync_ret = mpp_buffer_sync_ro_begin(src_buffer);
        if (sync_ret) {
            fprintf(stderr, "warning: mpp_buffer_sync_ro_begin failed ret=%d\n", sync_ret);
        }

        const auto *src = static_cast<const uint8_t *>(mpp_buffer_get_ptr(src_buffer));
        auto *dst = static_cast<uint8_t *>(mpp_buffer_get_ptr(frame_buf_));
        if (src_hor_stride == hor_stride_ && src_ver_stride == ver_stride_) {
            memcpy(dst, src, frame_size_);
        } else {
            const uint8_t *src_y = src;
            const uint8_t *src_uv = src_y + static_cast<size_t>(src_hor_stride) * src_ver_stride;
            uint8_t *dst_y = dst;
            uint8_t *dst_uv = dst_y + static_cast<size_t>(hor_stride_) * ver_stride_;
            if (src_hor_stride == hor_stride_) {
                memcpy(dst_y, src_y, static_cast<size_t>(hor_stride_) * opt_.height);
                memcpy(dst_uv, src_uv, static_cast<size_t>(hor_stride_) * (opt_.height / 2));
            } else {
                for (int y = 0; y < opt_.height; ++y) {
                    memcpy(dst_y + static_cast<size_t>(y) * hor_stride_,
                           src_y + static_cast<size_t>(y) * src_hor_stride,
                           opt_.width);
                }
                for (int y = 0; y < opt_.height / 2; ++y) {
                    memcpy(dst_uv + static_cast<size_t>(y) * hor_stride_,
                           src_uv + static_cast<size_t>(y) * src_hor_stride,
                           opt_.width);
                }
            }
        }

        sync_ret = mpp_buffer_sync_ro_end(src_buffer);
        if (sync_ret) {
            fprintf(stderr, "warning: mpp_buffer_sync_ro_end failed ret=%d\n", sync_ret);
        }
        sync_encoder_input_buffer();
        submit_nv12_frame(writer);
    }

    template <typename Writer>
    void encode_nv12_buffer_scale_cpu(MppBuffer src_buffer, int src_w, int src_h,
                                      int src_hor_stride, int src_ver_stride, Writer writer) {
        if (!frame_buf_) die("encoder CPU scale requires an encoder input buffer");
        (void)mpp_buffer_sync_ro_begin(src_buffer);
        const auto *src = static_cast<const uint8_t *>(mpp_buffer_get_ptr(src_buffer));
        auto *dst = static_cast<uint8_t *>(mpp_buffer_get_ptr(frame_buf_));
        scale_nv12_cpu(src, src_w, src_h, src_hor_stride, src_ver_stride,
                       dst, opt_.width, opt_.height, hor_stride_, ver_stride_);
        (void)mpp_buffer_sync_ro_end(src_buffer);
        sync_encoder_input_buffer();
        submit_nv12_frame(writer);
    }

    void request_idr() {
        MPP_RET ret = mpi_->control(ctx_, MPP_ENC_SET_IDR_FRAME, nullptr);
        if (ret) {
            fprintf(stderr, "warning: MPP_ENC_SET_IDR_FRAME failed ret=%d\n", ret);
        }
    }

private:
    struct PacketGuard {
        MppPacket pkt = nullptr;
        ~PacketGuard() {
            if (pkt) mpp_packet_deinit(&pkt);
        }
    };

    void sync_encoder_input_buffer() {
        if (!frame_buf_) return;
        MPP_RET ret = mpp_buffer_sync_end(frame_buf_);
        if (ret) {
            fprintf(stderr, "warning: mpp_buffer_sync_end encoder input failed ret=%d\n", ret);
        }
    }

    static int h264_level_for(int width, int height, int fps) {
        int mb_width = (width + 15) / 16;
        int mb_height = (height + 15) / 16;
        int frame_mbs = mb_width * mb_height;
        int mbps = frame_mbs * fps;

        if (frame_mbs <= 3600 && mbps <= 108000) return 31;
        if (frame_mbs <= 5120 && mbps <= 216000) return 32;
        if (frame_mbs <= 8192 && mbps <= 245760) return 40;
        return 42;
    }

    static int h265_level_for(int width, int height, int fps) {
        int pixels = width * height;
        int samples_per_sec = pixels * fps;
        if (pixels <= 1280 * 720 && samples_per_sec <= 1280 * 720 * 60) return 93;
        if (pixels <= 1920 * 1080 && samples_per_sec <= 1920 * 1080 * 30) return 120;
        if (pixels <= 1920 * 1080 && samples_per_sec <= 1920 * 1080 * 60) return 123;
        return 150;
    }

    MppCodingType coding_type() const {
        return opt_.video_codec == "h265" ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    }

    template <typename Writer>
    void submit_buffer(MppBuffer buffer, Writer writer) {
        mpp_frame_set_buffer(enc_frame_, buffer);
        MPP_RET ret = mpi_->encode_put_frame(ctx_, enc_frame_);
        mpp_frame_set_buffer(enc_frame_, nullptr);
        if (ret) die("encode_put_frame failed");

        PacketGuard out;
        ret = mpi_->encode_get_packet(ctx_, &out.pkt);
        if (ret) die("encode_get_packet failed");
        if (!out.pkt) die("MPP encoder returned no packet before the timeout");

        EncodedPacket encoded{
            static_cast<const uint8_t *>(mpp_packet_get_pos(out.pkt)),
            mpp_packet_get_length(out.pkt),
            {},
            0,
            false,
            nals_from_mpp_segments,
            out.pkt,
        };
        if (encoded.data && encoded.len > 0) writer(std::move(encoded));
    }

    void cleanup() {
        if (enc_frame_) mpp_frame_deinit(&enc_frame_);
        enc_frame_ = nullptr;
        if (ctx_) mpp_destroy(ctx_);
        ctx_ = nullptr;
        if (cfg_) mpp_enc_cfg_deinit(cfg_);
        cfg_ = nullptr;
        if (frame_buf_) mpp_buffer_put(frame_buf_);
        frame_buf_ = nullptr;
        if (buf_grp_) mpp_buffer_group_put(buf_grp_);
        buf_grp_ = nullptr;
    }

    void init() {
        if (allocate_input_buffer_) {
            const int group_type = cpu_writes_input_
                ? (MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE)
                : MPP_BUFFER_TYPE_DRM;
            if (mpp_buffer_group_get_internal(&buf_grp_, group_type)) {
                die("mpp encoder input buffer group init failed");
            }
            if (mpp_buffer_get(buf_grp_, &frame_buf_, frame_size_)) {
                die("mpp encoder input buffer allocation failed");
            }
        }

        if (mpp_frame_init(&enc_frame_)) die("mpp_frame_init failed");
        mpp_frame_set_width(enc_frame_, opt_.width);
        mpp_frame_set_height(enc_frame_, opt_.height);
        mpp_frame_set_hor_stride(enc_frame_, hor_stride_);
        mpp_frame_set_ver_stride(enc_frame_, ver_stride_);
        mpp_frame_set_fmt(enc_frame_, MPP_FMT_YUV420SP);

        if (mpp_create(&ctx_, &mpi_)) die("mpp_create failed");
        configure_mpp_timeouts(ctx_, mpi_);

        MppCodingType coding = coding_type();
        if (mpp_init(ctx_, MPP_CTX_ENC, coding)) die("mpp_init encoder failed");
        if (mpp_enc_cfg_init(&cfg_)) die("mpp_enc_cfg_init failed");
        if (mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_)) die("MPP_ENC_GET_CFG failed");

        mpp_enc_cfg_set_s32(cfg_, "codec:type", coding);
        mpp_enc_cfg_set_s32(cfg_, "prep:width", opt_.width);
        mpp_enc_cfg_set_s32(cfg_, "prep:height", opt_.height);
        mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", hor_stride_);
        mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", ver_stride_);
        mpp_enc_cfg_set_s32(cfg_, "prep:format", MPP_FMT_YUV420SP);
        mpp_enc_cfg_set_s32(cfg_, "prep:range", video_range_from_options(opt_));

        if (opt_.rc_mode == "vbr") {
            mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_VBR);
            mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", opt_.bitrate);
            mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", static_cast<int64_t>(opt_.bitrate) * 5 / 4);
            mpp_enc_cfg_set_s32(cfg_, "rc:bps_min",
                                std::max<int64_t>(64000, static_cast<int64_t>(opt_.bitrate) / 4));
        } else {
            mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
            mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", opt_.bitrate);
            mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", static_cast<int64_t>(opt_.bitrate) * 17 / 16);
            mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", static_cast<int64_t>(opt_.bitrate) * 15 / 16);
        }
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", opt_.fps);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denom", 1);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", opt_.fps);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denom", 1);
        mpp_enc_cfg_set_s32(cfg_, "rc:gop", opt_.gop);
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

        if (coding == MPP_VIDEO_CodingAVC) {
            mpp_enc_cfg_set_s32(cfg_, "h264:profile", 100);
            mpp_enc_cfg_set_s32(cfg_, "h264:level", h264_level_for(opt_.width, opt_.height, opt_.fps));
            mpp_enc_cfg_set_s32(cfg_, "h264:cabac_en", 1);
            mpp_enc_cfg_set_s32(cfg_, "h264:cabac_idc", 0);
            mpp_enc_cfg_set_s32(cfg_, "h264:trans8x8", 1);
        } else {
            mpp_enc_cfg_set_s32(cfg_, "h265:profile", 1);
            mpp_enc_cfg_set_s32(cfg_, "h265:level", h265_level_for(opt_.width, opt_.height, opt_.fps));
            mpp_enc_cfg_set_s32(cfg_, "h265:diff_cu_qp_delta_depth", 0);
        }

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
    bool allocate_input_buffer_ = true;
    bool cpu_writes_input_ = true;
    size_t frame_size_ = 0;
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppEncCfg cfg_ = nullptr;
    MppFrame enc_frame_ = nullptr;
    MppBufferGroup buf_grp_ = nullptr;
    MppBuffer frame_buf_ = nullptr;
};

} // namespace

std::vector<uint8_t> make_video_header(const Options &opt) {
    MppVideoEncoder enc(opt, false, false);
    return enc.read_header();
}

class StreamPipeline::Impl {
public:
    Impl(const Options &capture_opt, bool enable_default_encoder)
        : opt_(capture_opt),
          cap_hor_stride_(align16(capture_opt.width)),
          cap_ver_stride_(align16(capture_opt.height)) {
        cap_ = std::make_unique<V4L2Capture>(opt_);
        cap_->open_device();

        const bool mjpeg = opt_.input_format == "mjpeg";
        const bool use_rga = !mjpeg && opt_.yuyv_converter == "rga";
        if (mjpeg) {
            wake_fd_.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
            if (wake_fd_.get() < 0) die(std::string("eventfd failed: ") + strerror(errno));
            mppjpeg_ = std::make_unique<MppJpegDecoder>(opt_.width, opt_.height, opt_.mppjpeg_copy_output);
            if (!opt_.v4l2_dmabuf) mppjpeg_->prepare_copy_buffer(cap_->buffer_size());
            if (opt_.mppjpeg_copy_output) {
                fprintf(stderr, "mppjpeg copy-output mode enabled: copying into encoder input buffer\n");
            }
        } else {
            // Shared NV12 DMA-BUF produced by RGA or libyuv and fanned out to all active encoders.
            const size_t nv12_size = static_cast<size_t>(cap_hor_stride_) * cap_ver_stride_ * 3 / 2;
            shared_nv12_pool_ = std::make_unique<MppDmabufPool>(nv12_size, 1, !use_rga);
            shared_nv12_buf_ = shared_nv12_pool_->buffers()[0];

            if (use_rga) {
                yuyv_rga_capture_pool_ = std::make_unique<MppDmabufPool>(
                    cap_->buffer_size(), kV4l2BufferCount, false);
                RgaConverterConfig rga_cfg{};
                rga_cfg.library_path = opt_.rga_library;
                rga_cfg.width = opt_.width;
                rga_cfg.height = opt_.height;
                rga_cfg.src_stride_bytes = static_cast<int>(cap_->bytesperline());
                rga_cfg.dst_hor_stride = cap_hor_stride_;
                rga_cfg.dst_ver_stride = cap_ver_stride_;
                rga_cfg.dst_fd = mpp_buffer_get_fd(shared_nv12_buf_);
                rga_cfg.dst_size = nv12_size;
                yuyv_rga_ = std::make_unique<RgaYuyvToNv12Converter>(
                    rga_cfg, yuyv_rga_capture_pool_->buffers());
                fprintf(stderr, "YUYV converter: RGA dma-buf path enabled\n");
            }
        }

        if (enable_default_encoder) {
            enable_stream(0, opt_);
        }
    }

    ~Impl() {
        stop();
        stop_scale_thread();
        streams_.clear();
        cap_.reset();
        yuyv_rga_.reset();
        shared_nv12_pool_.reset();
    }

    const std::vector<uint8_t> &video_header() const {
        for (const auto &s : streams_) {
            if (s->id == 0) return s->header;
        }
        static const std::vector<uint8_t> empty;
        return empty;
    }

    const std::vector<uint8_t> &enable_stream(int stream_id, const Options &stream_opt) {
        for (const auto &s : streams_) {
            if (s->id == stream_id) return s->header;
        }

        auto out = std::make_unique<StreamOutput>();
        out->id = stream_id;
        out->opt = stream_opt;
        out->needs_scale = (stream_opt.width != opt_.width || stream_opt.height != opt_.height);
        out->copy_nv12 = (!out->needs_scale && opt_.input_format == "mjpeg" && opt_.mppjpeg_copy_output);
        out->fps_accum = opt_.fps - std::min(opt_.fps, stream_opt.fps);

        const bool need_own_buf = out->needs_scale || out->copy_nv12;
        if (out->needs_scale) {
#ifdef HAVE_RGA
            try {
                out->encoder = std::make_unique<MppVideoEncoder>(stream_opt, true, false);
                RgaScalerConfig scfg{};
                scfg.library_path = stream_opt.rga_library.empty() ? opt_.rga_library : stream_opt.rga_library;
                scfg.dst_width = stream_opt.width;
                scfg.dst_height = stream_opt.height;
                scfg.dst_hor_stride = out->encoder->hor_stride();
                scfg.dst_ver_stride = out->encoder->ver_stride();
                scfg.dst_fd = out->encoder->input_buffer_fd();
                out->rga_scaler = std::make_unique<RgaNv12Scaler>(scfg);
                fprintf(stderr, "stream id=%d RGA NV12 scaler enabled %dx%d -> %dx%d@%d %s %s %d bps\n",
                        stream_id, opt_.width, opt_.height, stream_opt.width, stream_opt.height,
                        stream_opt.fps, stream_opt.video_codec.c_str(), stream_opt.rc_mode.c_str(),
                        stream_opt.bitrate);
            } catch (const std::exception &e) {
                fprintf(stderr, "warning: RGA scaler unavailable (%s); falling back to CPU NV12 scale\n",
                        e.what());
                out->rga_scaler.reset();
                out->encoder = std::make_unique<MppVideoEncoder>(stream_opt, true, true);
            }
#else
            out->encoder = std::make_unique<MppVideoEncoder>(stream_opt, true, true);
            fprintf(stderr, "stream id=%d CPU NV12 scaler enabled %dx%d -> %dx%d@%d\n",
                    stream_id, opt_.width, opt_.height, stream_opt.width, stream_opt.height, stream_opt.fps);
#endif
        } else {
            out->encoder = std::make_unique<MppVideoEncoder>(stream_opt, need_own_buf, out->copy_nv12);
            fprintf(stderr, "stream id=%d encoder ready %dx%d@%d %s %s %d bps\n",
                    stream_id, stream_opt.width, stream_opt.height, stream_opt.fps,
                    stream_opt.video_codec.c_str(), stream_opt.rc_mode.c_str(), stream_opt.bitrate);
        }

        out->header = out->encoder->read_header();
        const auto &hdr = out->header;
        streams_.push_back(std::move(out));
        return hdr;
    }

    void disable_stream(int stream_id) {
        streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
                                      [stream_id](const std::unique_ptr<StreamOutput> &s) {
                                          return s->id == stream_id;
                                      }),
                       streams_.end());
    }

    bool has_stream(int stream_id) const {
        for (const auto &s : streams_) {
            if (s->id == stream_id) return true;
        }
        return false;
    }

    void start() {
        if (started_) return;
        try {
            const std::vector<MppBuffer> *capture_buffers = nullptr;
            if (mppjpeg_ && opt_.v4l2_dmabuf) {
                mppjpeg_->init_capture_buffers(cap_->buffer_size(), kV4l2BufferCount);
                capture_buffers = &mppjpeg_->capture_buffers();
            } else if (yuyv_rga_capture_pool_) {
                capture_buffers = &yuyv_rga_capture_pool_->buffers();
            }
            cap_->start(capture_buffers);
        } catch (const std::exception &e) {
            if (!mppjpeg_ || !opt_.v4l2_dmabuf) throw;
            fprintf(stderr, "V4L2 DMA-BUF unavailable (%s); retrying with mmap\n", e.what());
            cap_.reset();
            mppjpeg_->release_capture_buffers();
            opt_.v4l2_dmabuf = false;
            cap_ = std::make_unique<V4L2Capture>(opt_);
            cap_->open_device();
            mppjpeg_->prepare_copy_buffer(cap_->buffer_size());
            cap_->start();
        }
        if (mppjpeg_) start_decode_thread();
        started_ = true;
    }

    void stop() {
        if (!started_) return;
        stop_decode_thread();
        stop_scale_thread();
        cap_->stop();
        started_ = false;
    }

    void request_idr() {
        for (auto &s : streams_) s->encoder->request_idr();
    }

    void request_idr(int stream_id) {
        for (auto &s : streams_) {
            if (s->id == stream_id) {
                s->encoder->request_idr();
                return;
            }
        }
    }

    bool read_frame(const PacketWriter &writer, CaptureStats &stats) {
        return read_frame_multi(
            [&](int /*id*/, EncodedPacket pkt, uint64_t ts) { writer(std::move(pkt), ts); }, stats);
    }

    bool read_frame_multi(const MultiPacketWriter &writer, CaptureStats &stats) {
        if (mppjpeg_) {
            DecodedFrame df{};
            {
                std::unique_lock<std::mutex> lock(dec_mu_);
                dec_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return pending_frame_.has_value() || dec_failed_ ||
                           dec_stop_.load(std::memory_order_acquire);
                });
                stats.captured += dec_captured_;
                dec_captured_ = 0;
                stats.skipped += dec_skipped_;
                dec_skipped_ = 0;

                if (dec_failed_) {
                    auto err = dec_error_;
                    dec_error_ = nullptr;
                    dec_failed_ = false;
                    lock.unlock();
                    stop_decode_thread();
                    if (err) std::rethrow_exception(err);
                    return false;
                }
                if (!pending_frame_) return false;
                df = *pending_frame_;
                pending_frame_.reset();
                consumer_slot_ = static_cast<int>(df.slot);
            }

            struct SlotReleaseGuard {
                Impl *self;
                ~SlotReleaseGuard() {
                    std::lock_guard<std::mutex> lock(self->dec_mu_);
                    self->consumer_slot_ = -1;
                }
            } release_guard{this};

            return encode_nv12_frame(df.nv12_buf, df.hor_stride, df.ver_stride, df.timestamp_ns, writer);
        }

        auto lease = cap_->dequeue_frame();
        if (!lease) return false;

        ++stats.captured;
        const uint64_t timestamp_ns = lease->frame().timestamp_ns;

        if (opt_.yuyv_converter == "rga") {
            yuyv_rga_->convert(lease->frame());
        } else {
            const auto &f = lease->frame();
            convert_yuyv_to_nv12_buffer(f.data, f.bytesused, f.bytesperline,
                                        opt_.width, opt_.height, cap_hor_stride_, cap_ver_stride_,
                                        shared_nv12_buf_);
        }
        lease->requeue();
        return encode_nv12_frame(shared_nv12_buf_, cap_hor_stride_, cap_ver_stride_, timestamp_ns, writer);
    }

private:
    struct DecodedFrame {
        size_t slot = 0;
        MppBuffer nv12_buf = nullptr;
        int hor_stride = 0;
        int ver_stride = 0;
        uint64_t timestamp_ns = 0;
    };

    bool encode_nv12_frame(MppBuffer nv12_buf, int src_hor_stride, int src_ver_stride,
                           uint64_t timestamp_ns, const MultiPacketWriter &writer) {
        direct_batch_.clear();
        rga_scale_batch_.clear();
        cpu_scale_batch_.clear();
        for (auto &stream : streams_) {
            if (!stream->should_encode(opt_.fps)) continue;
            if (stream->needs_scale) {
                if (stream->rga_scaler) {
                    rga_scale_batch_.push_back(stream.get());
                } else {
                    cpu_scale_batch_.push_back(stream.get());
                }
            } else {
                direct_batch_.push_back(stream.get());
            }
        }
        if (direct_batch_.empty() && rga_scale_batch_.empty() && cpu_scale_batch_.empty()) {
            return false;
        }

        const int src_fd = mpp_buffer_get_fd(nv12_buf);
        bool any_encoded = false;
        auto encode_direct = [&](StreamOutput *stream) {
            auto stream_writer = [&](EncodedPacket packet) {
                writer(stream->id, std::move(packet), timestamp_ns);
                any_encoded = true;
            };
            if (stream->copy_nv12) {
                stream->encoder->encode_nv12_buffer_copy(nv12_buf, src_hor_stride, src_ver_stride, stream_writer);
            } else {
                stream->encoder->encode_nv12_buffer(nv12_buf, src_hor_stride, src_ver_stride, stream_writer);
            }
        };

        if (!direct_batch_.empty() && !rga_scale_batch_.empty()) {
            // Overlap RGA2 downscaling with the first direct VEPU encode: both read
            // the immutable NV12 DMA-BUF concurrently and write to separate buffers.
            start_async_rga_scale(src_fd, src_hor_stride, src_ver_stride);
            struct ScaleWaitGuard {
                Impl *self;
                bool waited = false;
                ~ScaleWaitGuard() {
                    if (!waited) self->wait_async_rga_scale_nothrow();
                }
            } scale_guard{this};

            encode_direct(direct_batch_[0]);
            scale_guard.waited = true;
            wait_async_rga_scale();

            for (size_t i = 1; i < direct_batch_.size(); ++i) {
                encode_direct(direct_batch_[i]);
            }
            for (StreamOutput *stream : rga_scale_batch_) {
                auto stream_writer = [&](EncodedPacket packet) {
                    writer(stream->id, std::move(packet), timestamp_ns);
                    any_encoded = true;
                };
                stream->encoder->submit_nv12_frame(stream_writer);
            }
        } else {
            for (StreamOutput *stream : direct_batch_) {
                encode_direct(stream);
            }
            for (StreamOutput *stream : rga_scale_batch_) {
                stream->rga_scaler->scale(src_fd, opt_.width, opt_.height, src_hor_stride, src_ver_stride);
                auto stream_writer = [&](EncodedPacket packet) {
                    writer(stream->id, std::move(packet), timestamp_ns);
                    any_encoded = true;
                };
                stream->encoder->submit_nv12_frame(stream_writer);
            }
        }

        for (StreamOutput *stream : cpu_scale_batch_) {
            auto stream_writer = [&](EncodedPacket packet) {
                writer(stream->id, std::move(packet), timestamp_ns);
                any_encoded = true;
            };
            stream->encoder->encode_nv12_buffer_scale_cpu(
                nv12_buf, opt_.width, opt_.height, src_hor_stride, src_ver_stride, stream_writer);
        }
        return any_encoded;
    }

    void start_async_rga_scale(int src_fd, int src_hor_stride, int src_ver_stride) {
        if (!scale_thread_.joinable()) {
            scale_stop_ = false;
            scale_has_job_ = false;
            scale_done_ = false;
            scale_error_ = nullptr;
            scale_thread_ = std::thread([this] { scale_loop(); });
        }
        {
            std::lock_guard<std::mutex> lock(scale_mu_);
            scale_src_fd_ = src_fd;
            scale_src_hor_stride_ = src_hor_stride;
            scale_src_ver_stride_ = src_ver_stride;
            scale_error_ = nullptr;
            scale_done_ = false;
            scale_has_job_ = true;
        }
        scale_cv_.notify_one();
    }

    void wait_async_rga_scale() {
        std::unique_lock<std::mutex> lock(scale_mu_);
        scale_cv_.wait(lock, [this] { return scale_done_; });
        auto err = scale_error_;
        scale_error_ = nullptr;
        scale_done_ = false;
        if (err) std::rethrow_exception(err);
    }

    void wait_async_rga_scale_nothrow() noexcept {
        std::unique_lock<std::mutex> lock(scale_mu_);
        scale_cv_.wait(lock, [this] { return scale_done_; });
        scale_error_ = nullptr;
        scale_done_ = false;
    }

    void stop_scale_thread() {
        if (!scale_thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(scale_mu_);
            scale_stop_ = true;
        }
        scale_cv_.notify_all();
        scale_thread_.join();
        scale_stop_ = false;
        scale_has_job_ = false;
        scale_done_ = false;
        scale_error_ = nullptr;
    }

    void scale_loop() {
        for (;;) {
            int src_fd = -1;
            int src_hor_stride = 0;
            int src_ver_stride = 0;
            {
                std::unique_lock<std::mutex> lock(scale_mu_);
                scale_cv_.wait(lock, [this] { return scale_has_job_ || scale_stop_; });
                if (scale_stop_ && !scale_has_job_) return;
                src_fd = scale_src_fd_;
                src_hor_stride = scale_src_hor_stride_;
                src_ver_stride = scale_src_ver_stride_;
                scale_has_job_ = false;
            }
            std::exception_ptr err;
            try {
                for (StreamOutput *stream : rga_scale_batch_) {
                    stream->rga_scaler->scale(src_fd, opt_.width, opt_.height, src_hor_stride, src_ver_stride);
                }
            } catch (...) {
                err = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(scale_mu_);
                scale_error_ = err;
                scale_done_ = true;
            }
            scale_cv_.notify_one();
        }
    }

    size_t pick_free_slot_locked() const {
        const size_t n = MppJpegDecoder::frame_pool_size();
        for (size_t s = 0; s < n; ++s) {
            if (static_cast<int>(s) == consumer_slot_) continue;
            if (pending_frame_ && pending_frame_->slot == s) continue;
            return s;
        }
        die("no free MJPEG decoder slot available");
    }

    struct UniqueFd {
        int fd = -1;
        UniqueFd() = default;
        ~UniqueFd() { reset(-1); }
        UniqueFd(const UniqueFd &) = delete;
        UniqueFd &operator=(const UniqueFd &) = delete;
        int get() const { return fd; }
        void reset(int new_fd) {
            if (fd >= 0) close(fd);
            fd = new_fd;
        }
    };

    void start_decode_thread() {
        stop_decode_thread();
        if (wake_fd_.get() >= 0) {
            uint64_t dummy = 0;
            while (read(wake_fd_.get(), &dummy, sizeof(dummy)) > 0) {}
        }
        {
            std::lock_guard<std::mutex> lock(dec_mu_);
            pending_frame_.reset();
            consumer_slot_ = -1;
            dec_captured_ = 0;
            dec_skipped_ = 0;
            dec_failed_ = false;
            dec_error_ = nullptr;
        }
        dec_stop_.store(false, std::memory_order_release);
        decode_thread_ = std::thread([this] { decode_loop(); });
    }

    void stop_decode_thread() {
        dec_stop_.store(true, std::memory_order_release);
        if (wake_fd_.get() >= 0) {
            const uint64_t one = 1;
            (void)write(wake_fd_.get(), &one, sizeof(one));
        }
        dec_cv_.notify_all();
        if (decode_thread_.joinable()) decode_thread_.join();
        std::lock_guard<std::mutex> lock(dec_mu_);
        pending_frame_.reset();
        consumer_slot_ = -1;
    }

    void decode_loop() {
        uint64_t total_captured = 0;
        uint64_t total_corrupt = 0;
        try {
            while (!dec_stop_.load(std::memory_order_acquire)) {
                auto lease = cap_->dequeue_frame(wake_fd_.get());
                if (!lease) continue;

                const uint64_t timestamp_ns = lease->frame().timestamp_ns;
                ++total_captured;
                size_t slot = 0;
                {
                    std::lock_guard<std::mutex> lock(dec_mu_);
                    ++dec_captured_;
                    slot = pick_free_slot_locked();
                }

                MppFrame frame = mppjpeg_->decode_to_frame(lease->frame(), slot);
                lease->requeue();
                if (!frame) {
                    ++total_corrupt;
                    {
                        std::lock_guard<std::mutex> lock(dec_mu_);
                        ++dec_skipped_;
                    }
                    if (total_corrupt <= 10 || total_corrupt % opt_.fps == 0) {
                        fprintf(stderr, "skipped corrupt/unsupported MJPEG frame captured=%llu skipped=%llu\n",
                                static_cast<unsigned long long>(total_captured),
                                static_cast<unsigned long long>(total_corrupt));
                    }
                    continue;
                }

                DecodedFrame df{};
                df.slot = slot;
                df.nv12_buf = mpp_frame_get_buffer(frame);
                df.hor_stride = static_cast<int>(mpp_frame_get_hor_stride(frame));
                df.ver_stride = static_cast<int>(mpp_frame_get_ver_stride(frame));
                df.timestamp_ns = timestamp_ns;

                {
                    std::lock_guard<std::mutex> lock(dec_mu_);
                    if (pending_frame_) {
                        ++dec_skipped_;
                    }
                    pending_frame_ = df;
                }
                dec_cv_.notify_one();
            }
        } catch (...) {
            if (!dec_stop_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(dec_mu_);
                dec_error_ = std::current_exception();
                dec_failed_ = true;
            }
            dec_cv_.notify_all();
        }
    }

    struct StreamOutput {
        int id = 0;
        Options opt;
        bool needs_scale = false;
        bool copy_nv12 = false;
        int fps_accum = 0;
        std::unique_ptr<RgaNv12Scaler> rga_scaler;
        std::unique_ptr<MppVideoEncoder> encoder;
        std::vector<uint8_t> header;

        bool should_encode(int capture_fps) {
            if (opt.fps >= capture_fps) return true;
            fps_accum += opt.fps;
            if (fps_accum < capture_fps) return false;
            fps_accum -= capture_fps;
            return true;
        }
    };

    Options opt_;
    int cap_hor_stride_ = 0;
    int cap_ver_stride_ = 0;
    std::unique_ptr<V4L2Capture> cap_;
    std::unique_ptr<MppJpegDecoder> mppjpeg_;
    std::unique_ptr<MppDmabufPool> yuyv_rga_capture_pool_;
    std::unique_ptr<MppDmabufPool> shared_nv12_pool_;
    MppBuffer shared_nv12_buf_ = nullptr;
    std::unique_ptr<RgaYuyvToNv12Converter> yuyv_rga_;
    std::vector<std::unique_ptr<StreamOutput>> streams_;
    bool started_ = false;
    UniqueFd wake_fd_;
    std::atomic<bool> dec_stop_{false};
    std::mutex dec_mu_;
    std::condition_variable dec_cv_;
    std::optional<DecodedFrame> pending_frame_;
    int consumer_slot_ = -1;
    uint64_t dec_captured_ = 0;
    uint64_t dec_skipped_ = 0;
    bool dec_failed_ = false;
    std::exception_ptr dec_error_;
    std::thread decode_thread_;
    std::vector<StreamOutput *> direct_batch_;
    std::vector<StreamOutput *> rga_scale_batch_;
    std::vector<StreamOutput *> cpu_scale_batch_;
    std::mutex scale_mu_;
    std::condition_variable scale_cv_;
    bool scale_stop_ = false;
    bool scale_has_job_ = false;
    bool scale_done_ = false;
    int scale_src_fd_ = -1;
    int scale_src_hor_stride_ = 0;
    int scale_src_ver_stride_ = 0;
    std::exception_ptr scale_error_;
    std::thread scale_thread_;
};

StreamPipeline::StreamPipeline(const Options &capture_opt, bool enable_default_encoder)
    : impl_(std::make_unique<Impl>(capture_opt, enable_default_encoder)) {}
StreamPipeline::~StreamPipeline() = default;
const std::vector<uint8_t> &StreamPipeline::video_header() const { return impl_->video_header(); }
const std::vector<uint8_t> &StreamPipeline::enable_stream(int stream_id, const Options &stream_opt) {
    return impl_->enable_stream(stream_id, stream_opt);
}
void StreamPipeline::disable_stream(int stream_id) { impl_->disable_stream(stream_id); }
bool StreamPipeline::has_stream(int stream_id) const { return impl_->has_stream(stream_id); }
void StreamPipeline::start() { impl_->start(); }
void StreamPipeline::stop() { impl_->stop(); }
void StreamPipeline::request_idr() { impl_->request_idr(); }
void StreamPipeline::request_idr(int stream_id) { impl_->request_idr(stream_id); }
bool StreamPipeline::read_frame(const PacketWriter &writer, CaptureStats &stats) {
    return impl_->read_frame(writer, stats);
}
bool StreamPipeline::read_frame_multi(const MultiPacketWriter &writer, CaptureStats &stats) {
    return impl_->read_frame_multi(writer, stats);
}
