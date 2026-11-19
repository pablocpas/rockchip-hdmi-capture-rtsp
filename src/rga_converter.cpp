#include "rga_converter.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef HAVE_RGA
#include <RgaUtils.h>
#include <im2d.h>
#endif

namespace {

[[noreturn]] void rga_die(const std::string &msg) {
    throw std::runtime_error(msg);
}

#ifdef HAVE_RGA
class RgaApi {
public:
    explicit RgaApi(const std::string &explicit_path) {
        std::vector<std::string> candidates;
        if (!explicit_path.empty()) candidates.push_back(explicit_path);
        if (const char *env = getenv("RGA_LIBRARY"); env && *env) candidates.emplace_back(env);
        candidates.emplace_back("librga.so");
        candidates.emplace_back("/usr/local/lib/librga.so");
        candidates.emplace_back("/usr/lib/aarch64-linux-gnu/librga.so");

        std::string errors;
        for (const auto &path : candidates) {
            handle_.reset(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL));
            if (handle_) {
                loaded_path_ = path;
                break;
            }
            if (const char *err = dlerror()) {
                errors += path + ": " + err + "\n";
            }
        }
        if (!handle_) rga_die("failed to load librga.so\n" + errors);

        importbuffer_fd_ = load<ImportBufferFdFn>("importbuffer_fd");
        releasebuffer_handle_ = load<ReleaseBufferHandleFn>("releasebuffer_handle");
        wrapbuffer_handle_t_ = load<WrapBufferHandleFn>("wrapbuffer_handle_t");
        imcvtcolor_t_ = load<ImCvtColorFn>("imcvtcolor_t");
        imresize_t_ = load<ImResizeFn>("imresize_t");
        imStrError_t_ = load<ImStrErrorFn>("imStrError_t");

        fprintf(stderr, "RGA runtime library=%s\n", loaded_path_.c_str());
    }

    rga_buffer_handle_t import_fd(int fd, uint32_t width, uint32_t height, uint32_t format) const {
        im_handle_param_t param{};
        param.width = width;
        param.height = height;
        param.format = format;
        return importbuffer_fd_(fd, &param);
    }

    IM_STATUS release_handle(rga_buffer_handle_t handle) const {
        return releasebuffer_handle_(handle);
    }

    rga_buffer_t wrap_handle(rga_buffer_handle_t handle,
                             int width,
                             int height,
                             int wstride,
                             int hstride,
                             int format) const {
        return wrapbuffer_handle_t_(handle, width, height, wstride, hstride, format);
    }

    IM_STATUS cvtcolor(rga_buffer_t src, rga_buffer_t dst, int sfmt, int dfmt, int mode, int sync) const {
        return imcvtcolor_t_(src, dst, sfmt, dfmt, mode, sync);
    }

    IM_STATUS resize(rga_buffer_t src, rga_buffer_t dst, double fx, double fy, int interp, int sync) const {
        return imresize_t_(src, dst, fx, fy, interp, sync);
    }

    const char *strerror(IM_STATUS status) const {
        return imStrError_t_(status);
    }

private:
    using ImportBufferFdFn = rga_buffer_handle_t (*)(int, im_handle_param_t *);
    using ReleaseBufferHandleFn = IM_STATUS (*)(rga_buffer_handle_t);
    using WrapBufferHandleFn = rga_buffer_t (*)(rga_buffer_handle_t, int, int, int, int, int);
    using ImCvtColorFn = IM_STATUS (*)(rga_buffer_t, rga_buffer_t, int, int, int, int);
    using ImResizeFn = IM_STATUS (*)(const rga_buffer_t, rga_buffer_t, double, double, int, int);
    using ImStrErrorFn = const char *(*)(IM_STATUS);

    template <typename Fn>
    Fn load(const char *name) {
        dlerror();
        void *sym = dlsym(handle_.get(), name);
        if (!sym) {
            const char *err = dlerror();
            rga_die(std::string("failed to load RGA symbol ") + name + ": " + (err ? err : "unknown error"));
        }
        return reinterpret_cast<Fn>(sym);
    }

    struct LibraryCloser {
        void operator()(void *handle) const { dlclose(handle); }
    };

    std::unique_ptr<void, LibraryCloser> handle_;
    std::string loaded_path_;
    ImportBufferFdFn importbuffer_fd_ = nullptr;
    ReleaseBufferHandleFn releasebuffer_handle_ = nullptr;
    WrapBufferHandleFn wrapbuffer_handle_t_ = nullptr;
    ImCvtColorFn imcvtcolor_t_ = nullptr;
    ImResizeFn imresize_t_ = nullptr;
    ImStrErrorFn imStrError_t_ = nullptr;
};
#endif

} // namespace

struct RgaYuyvToNv12Converter::Impl {
#ifdef HAVE_RGA
    explicit Impl(const RgaConverterConfig &cfg, const std::vector<MppBuffer> &src_buffers)
        : api(cfg.library_path) {
        if (cfg.width <= 0 || cfg.height <= 0 || cfg.src_stride_bytes <= 0 ||
            cfg.src_stride_bytes % 2 != 0) {
            rga_die("invalid YUYV dimensions or source stride for RGA");
        }
        const auto row_size = static_cast<size_t>(cfg.width) * 2;
        const auto stride = static_cast<size_t>(cfg.src_stride_bytes);
        if (stride < row_size) rga_die("invalid YUYV source stride for RGA");
        min_source_size = static_cast<size_t>(cfg.height - 1) * stride + row_size;
        if (cfg.dst_fd < 0 || cfg.dst_size == 0) {
            rga_die("invalid encoder input dma-buf for RGA");
        }

        try {
            sources.reserve(src_buffers.size());
            const int src_stride_pixels = cfg.src_stride_bytes / 2;
            for (MppBuffer buf : src_buffers) {
                const int fd = mpp_buffer_get_fd(buf);
                const auto handle = api.import_fd(fd, src_stride_pixels, cfg.height, RK_FORMAT_YUYV_422);
                if (!handle) rga_die("RGA import source dma-buf failed");
                sources.push_back({handle, {}});
                sources.back().descriptor = api.wrap_handle(handle, cfg.width, cfg.height,
                                                            src_stride_pixels, cfg.height, RK_FORMAT_YUYV_422);
                if (sources.back().descriptor.width <= 0 || sources.back().descriptor.height <= 0) {
                    rga_die("RGA source buffer descriptor failed");
                }
            }

            destination.handle = api.import_fd(cfg.dst_fd, cfg.dst_hor_stride, cfg.dst_ver_stride, RK_FORMAT_YCbCr_420_SP);
            if (!destination.handle) rga_die("RGA import encoder dma-buf failed");
            destination.descriptor = api.wrap_handle(destination.handle, cfg.width, cfg.height,
                                                     cfg.dst_hor_stride, cfg.dst_ver_stride, RK_FORMAT_YCbCr_420_SP);
            if (destination.descriptor.width <= 0 || destination.descriptor.height <= 0) {
                rga_die("RGA encoder buffer descriptor failed");
            }
        } catch (...) {
            release_handles();
            throw;
        }
    }

    ~Impl() { release_handles(); }

    void release_handles() {
        for (const auto &source : sources) api.release_handle(source.handle);
        if (destination.handle) api.release_handle(destination.handle);
    }

    void convert(const CapturedFrame &frame) {
        if (frame.index >= sources.size()) rga_die("RGA source buffer index out of range");
        if (frame.bytesused < min_source_size) rga_die("short YUYV frame");

        const auto ret = api.cvtcolor(sources[frame.index].descriptor,
                                      destination.descriptor,
                                      RK_FORMAT_YUYV_422,
                                      RK_FORMAT_YCbCr_420_SP,
                                      IM_COLOR_SPACE_DEFAULT,
                                      1);
        if (ret != IM_STATUS_SUCCESS) {
            std::ostringstream oss;
            oss << "RGA YUYV->NV12 failed: " << api.strerror(ret);
            rga_die(oss.str());
        }
    }

    struct ImportedBuffer {
        rga_buffer_handle_t handle = 0;
        rga_buffer_t descriptor{};
    };

    RgaApi api;
    std::vector<ImportedBuffer> sources;
    ImportedBuffer destination;
    size_t min_source_size = 0;
#else
    Impl(const RgaConverterConfig &, const std::vector<MppBuffer> &) {
        rga_die("RGA converter requested but this binary was built without librga headers");
    }

    void convert(const CapturedFrame &) {}
#endif
};

RgaYuyvToNv12Converter::RgaYuyvToNv12Converter(const RgaConverterConfig &cfg,
                                               const std::vector<MppBuffer> &src_buffers)
    : impl_(std::make_unique<Impl>(cfg, src_buffers)) {}

RgaYuyvToNv12Converter::~RgaYuyvToNv12Converter() = default;

void RgaYuyvToNv12Converter::convert(const CapturedFrame &frame) {
    impl_->convert(frame);
}

struct RgaNv12Scaler::Impl {
#ifdef HAVE_RGA
    explicit Impl(const RgaScalerConfig &cfg) : api(cfg.library_path) {
        if (cfg.dst_width <= 0 || cfg.dst_height <= 0 || cfg.dst_fd < 0) {
            rga_die("invalid RGA NV12 scaler destination config");
        }
        destination.handle = api.import_fd(cfg.dst_fd, cfg.dst_hor_stride, cfg.dst_ver_stride,
                                           RK_FORMAT_YCbCr_420_SP);
        if (!destination.handle) rga_die("RGA import scaler destination dma-buf failed");
        destination.descriptor = api.wrap_handle(destination.handle, cfg.dst_width, cfg.dst_height,
                                                 cfg.dst_hor_stride, cfg.dst_ver_stride,
                                                 RK_FORMAT_YCbCr_420_SP);
        if (destination.descriptor.width <= 0 || destination.descriptor.height <= 0) {
            api.release_handle(destination.handle);
            destination.handle = 0;
            rga_die("RGA scaler destination descriptor failed");
        }
    }

    ~Impl() {
        for (const auto &entry : cached_sources) api.release_handle(entry.handle);
        if (destination.handle) api.release_handle(destination.handle);
    }

    void scale(int src_fd, int src_width, int src_height, int src_hor_stride, int src_ver_stride) {
        if (src_fd < 0) rga_die("RGA NV12 scaler requires a valid source dma-buf fd");
        const rga_buffer_t *src_desc = nullptr;
        for (const auto &entry : cached_sources) {
            if (entry.fd == src_fd && entry.width == src_width && entry.height == src_height &&
                entry.hor_stride == src_hor_stride && entry.ver_stride == src_ver_stride) {
                src_desc = &entry.descriptor;
                break;
            }
        }
        if (!src_desc) {
            const auto handle = api.import_fd(src_fd, src_hor_stride, src_ver_stride,
                                              RK_FORMAT_YCbCr_420_SP);
            if (!handle) rga_die("RGA import scaler source dma-buf failed");
            auto desc = api.wrap_handle(handle, src_width, src_height, src_hor_stride, src_ver_stride,
                                        RK_FORMAT_YCbCr_420_SP);
            if (desc.width <= 0 || desc.height <= 0) {
                api.release_handle(handle);
                rga_die("RGA scaler source descriptor failed");
            }
            cached_sources.push_back({src_fd, src_width, src_height, src_hor_stride, src_ver_stride, handle, desc});
            src_desc = &cached_sources.back().descriptor;
        }

        const auto ret = api.resize(*src_desc, destination.descriptor, 0.0, 0.0, INTER_LINEAR, 1);
        if (ret != IM_STATUS_SUCCESS) {
            std::ostringstream oss;
            oss << "RGA NV12 scale failed: " << api.strerror(ret);
            rga_die(oss.str());
        }
    }

    struct CachedSource {
        int fd = -1;
        int width = 0;
        int height = 0;
        int hor_stride = 0;
        int ver_stride = 0;
        rga_buffer_handle_t handle = 0;
        rga_buffer_t descriptor{};
    };

    struct ImportedBuffer {
        rga_buffer_handle_t handle = 0;
        rga_buffer_t descriptor{};
    };

    RgaApi api;
    std::vector<CachedSource> cached_sources;
    ImportedBuffer destination;
#else
    explicit Impl(const RgaScalerConfig &) {
        rga_die("RGA scaler requested but this binary was built without librga headers");
    }

    void scale(int, int, int, int, int) {}
#endif
};

RgaNv12Scaler::RgaNv12Scaler(const RgaScalerConfig &cfg)
    : impl_(std::make_unique<Impl>(cfg)) {}

RgaNv12Scaler::~RgaNv12Scaler() = default;

void RgaNv12Scaler::scale(int src_fd, int src_width, int src_height,
                          int src_hor_stride, int src_ver_stride) {
    impl_->scale(src_fd, src_width, src_height, src_hor_stride, src_ver_stride);
}
