#include "rga_converter.hpp"

#include <dlfcn.h>

#include <sstream>
#include <stdexcept>

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
            handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
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
        imStrError_t_ = load<ImStrErrorFn>("imStrError_t");

        fprintf(stderr, "RGA runtime library=%s\n", loaded_path_.c_str());
    }

    ~RgaApi() {
        if (handle_) dlclose(handle_);
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

    const char *strerror(IM_STATUS status) const {
        return imStrError_t_(status);
    }

private:
    using ImportBufferFdFn = rga_buffer_handle_t (*)(int, im_handle_param_t *);
    using ReleaseBufferHandleFn = IM_STATUS (*)(rga_buffer_handle_t);
    using WrapBufferHandleFn = rga_buffer_t (*)(rga_buffer_handle_t, int, int, int, int, int);
    using ImCvtColorFn = IM_STATUS (*)(rga_buffer_t, rga_buffer_t, int, int, int, int);
    using ImStrErrorFn = const char *(*)(IM_STATUS);

    template <typename Fn>
    Fn load(const char *name) {
        dlerror();
        void *sym = dlsym(handle_, name);
        if (!sym) {
            const char *err = dlerror();
            rga_die(std::string("failed to load RGA symbol ") + name + ": " + (err ? err : "unknown error"));
        }
        return reinterpret_cast<Fn>(sym);
    }

    void *handle_ = nullptr;
    std::string loaded_path_;
    ImportBufferFdFn importbuffer_fd_ = nullptr;
    ReleaseBufferHandleFn releasebuffer_handle_ = nullptr;
    WrapBufferHandleFn wrapbuffer_handle_t_ = nullptr;
    ImCvtColorFn imcvtcolor_t_ = nullptr;
    ImStrErrorFn imStrError_t_ = nullptr;
};
#endif

} // namespace

struct RgaYuyvToNv12Converter::Impl {
#ifdef HAVE_RGA
    explicit Impl(const RgaConverterConfig &cfg, const std::vector<MppBuffer> &src_buffers)
        : cfg(cfg),
          api(cfg.library_path),
          src_stride_pixels(cfg.src_stride_bytes / 2) {
        if (cfg.src_stride_bytes <= 0 || cfg.src_stride_bytes % 2 != 0) {
            rga_die("invalid YUYV source stride for RGA");
        }
        if (cfg.dst_fd < 0 || cfg.dst_size == 0) {
            rga_die("invalid encoder input dma-buf for RGA");
        }

        src_handles.reserve(src_buffers.size());
        for (MppBuffer buf : src_buffers) {
            int fd = mpp_buffer_get_fd(buf);
            rga_buffer_handle_t handle = api.import_fd(fd,
                                                       static_cast<uint32_t>(src_stride_pixels),
                                                       static_cast<uint32_t>(cfg.height),
                                                       RK_FORMAT_YUYV_422);
            if (!handle) rga_die("RGA import source dma-buf failed");
            src_handles.push_back(handle);
        }

        dst_handle = api.import_fd(cfg.dst_fd,
                                   static_cast<uint32_t>(cfg.dst_hor_stride),
                                   static_cast<uint32_t>(cfg.dst_ver_stride),
                                   RK_FORMAT_YCbCr_420_SP);
        if (!dst_handle) rga_die("RGA import encoder dma-buf failed");
    }

    ~Impl() {
        for (auto handle : src_handles) {
            if (handle) api.release_handle(handle);
        }
        if (dst_handle) api.release_handle(dst_handle);
    }

    void convert(const CapturedFrame &frame) {
        if (frame.index >= src_handles.size()) rga_die("RGA source buffer index out of range");
        if (frame.bytesused < static_cast<size_t>(cfg.width) * cfg.height * 2) {
            rga_die("short YUYV frame");
        }

        rga_buffer_t src = api.wrap_handle(src_handles[frame.index],
                                           cfg.width,
                                           cfg.height,
                                           src_stride_pixels,
                                           cfg.height,
                                           RK_FORMAT_YUYV_422);
        rga_buffer_t dst = api.wrap_handle(dst_handle,
                                           cfg.width,
                                           cfg.height,
                                           cfg.dst_hor_stride,
                                           cfg.dst_ver_stride,
                                           RK_FORMAT_YCbCr_420_SP);

        IM_STATUS ret = api.cvtcolor(src,
                                     dst,
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

    RgaConverterConfig cfg;
    RgaApi api;
    std::vector<rga_buffer_handle_t> src_handles;
    rga_buffer_handle_t dst_handle = 0;
    int src_stride_pixels = 0;
#else
    Impl(const RgaConverterConfig &, const std::vector<MppBuffer> &) {
        rga_die("RGA converter requested but this binary was built without librga headers");
    }

    void convert(const CapturedFrame &) {}
#endif
};

RgaYuyvToNv12Converter::RgaYuyvToNv12Converter(const RgaConverterConfig &cfg,
                                               const std::vector<MppBuffer> &src_buffers)
    : impl_(new Impl(cfg, src_buffers)) {}

RgaYuyvToNv12Converter::~RgaYuyvToNv12Converter() {
    delete impl_;
}

void RgaYuyvToNv12Converter::convert(const CapturedFrame &frame) {
    impl_->convert(frame);
}
