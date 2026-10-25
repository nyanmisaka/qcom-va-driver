// SPDX-License-Identifier: MIT
// Hardware integration: scalar reference, PRIME2 identity and DMA-BUF lifetime.
// Pass cdsp or vulkan to select a backend; --bench runs warmed VAAPI timings.
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <drm_fourcc.h>
#include "../../src/cdsp/filter.h"
#ifndef VPP_NO_CDSP
#include "../../src/cdsp/client.h"
#endif
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#ifndef VPP_NO_CDSP
#include <misc/fastrpc.h>
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <chrono>
#include <vulkan/vulkan.h>

static int input_fd = -1, output_fd = -1, direct_calls = 0;
static int maps = 0, unmaps = 0;
static bool gpu = false, benchmark = false;
static unsigned imports = 0;
static bool saw_input = false, saw_output = false;
static bool same_buffer(int a, int b) {
    struct stat x{}, y{};
    return a >= 0 && b >= 0 && !fstat(a, &x) && !fstat(b, &y) && x.st_dev == y.st_dev &&
           x.st_ino == y.st_ino;
}
extern "C" VkResult vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                     const VkAllocationCallbacks *callbacks,
                                     VkDeviceMemory *memory) {
    static auto real = reinterpret_cast<PFN_vkAllocateMemory>(dlsym(RTLD_NEXT, "vkAllocateMemory"));
    for (auto next = static_cast<const VkBaseInStructure *>(info->pNext); next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
            auto fd = reinterpret_cast<const VkImportMemoryFdInfoKHR *>(next);
            if (fd->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
                ++imports;
                saw_input |= same_buffer(fd->fd, input_fd);
                saw_output |= same_buffer(fd->fd, output_fd);
            }
        }
    return real(device, info, callbacks, memory);
}
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    static auto real =
        reinterpret_cast<int (*)(int, unsigned long, ...)>(dlsym(RTLD_NEXT, "ioctl"));
    bool both = false;
#ifndef VPP_NO_CDSP
    if (request == FASTRPC_IOCTL_INVOKE && arg) {
        auto *call = static_cast<fastrpc_invoke *>(arg);
        auto *buffers = reinterpret_cast<fastrpc_invoke_args *>(call->args);
        unsigned n = ((call->sc >> 16) & 255) + ((call->sc >> 8) & 255) + ((call->sc >> 4) & 15) +
                     (call->sc & 15);
        bool in = false, out = false;
        for (unsigned i = 0; i < n; ++i) {
            in |= same_buffer(buffers[i].fd, input_fd);
            out |= same_buffer(buffers[i].fd, output_fd);
        }
        both = in && out;
    }
#endif
    int result = real(fd, request, arg);
#ifndef VPP_NO_CDSP
    if (!result && request == FASTRPC_IOCTL_MEM_MAP)
        ++maps;
    if (!result && request == FASTRPC_IOCTL_MEM_UNMAP)
        ++unmaps;
#endif
    if (!result && both)
        ++direct_calls;
    return result;
}
static void require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
static void va(VAStatus status) {
    if (status) {
        std::fprintf(stderr, "VA: %s\n", vaErrorStr(status));
        std::exit(1);
    }
}
static void sync(int fd, uint64_t flags) {
    dma_buf_sync s{flags};
    require(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0, "DMA-BUF CPU sync");
}
struct Allocation {
    VADRMPRIMESurfaceDescriptor desc{};
    uint8_t *data;
    Allocation(unsigned w, unsigned h, unsigned format = VA_FOURCC_NV12, bool tight = false) {
        auto &obj = desc.objects[0];
        auto &l = desc.layers[0];
        desc.fourcc = format;
        l.drm_format = format == VA_FOURCC_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
        desc.width = w;
        desc.height = h;
        desc.num_objects = desc.num_layers = 1;
        l.num_planes = 2;
        unsigned row_bytes = w * (format == VA_FOURCC_P010 ? 2 : 1);
        l.pitch[0] = l.pitch[1] = tight ? row_bytes : (row_bytes + 127) & ~127u;
        // Exercise both padded imports and planes ending at the DMA-BUF boundary.
        l.offset[0] = tight ? 0 : 128;
        l.offset[1] = l.offset[0] + l.pitch[0] * (tight ? h : (h + 31) & ~31u);
        obj.size = (l.offset[1] + l.pitch[1] * ((h + 1) / 2) + 4095) & ~4095u;
        obj.drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        require(heap >= 0, "open system heap");
        dma_heap_allocation_data a{};
        a.len = obj.size;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        require(!ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a), "allocate DMA-BUF");
        close(heap);
        obj.fd = a.fd;
        data = static_cast<uint8_t *>(
            mmap(nullptr, obj.size, PROT_READ | PROT_WRITE, MAP_SHARED, obj.fd, 0));
        require(data != MAP_FAILED, "map allocation");
    }
    ~Allocation() {
        munmap(data, desc.objects[0].size);
        close(desc.objects[0].fd);
    }
};
static VASurfaceID import(VADisplay display, VADRMPRIMESurfaceDescriptor &desc) {
    VASurfaceAttrib attrs[2]{};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    auto owned = desc;
    owned.objects[0].fd = dup(desc.objects[0].fd);
    require(owned.objects[0].fd >= 0, "duplicate descriptor fd");
    attrs[1].value.value.p = &owned;
    VASurfaceID id;
    va(vaCreateSurfaces(
        display, desc.fourcc == VA_FOURCC_P010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420,
        desc.width, desc.height, &id, 1, attrs, 2));
    close(owned.objects[0].fd); // Driver must own its own fd after import.
    return id;
}
struct Case {
    unsigned sw, sh, dw, dh;
    bool crop;
    unsigned src_format = VA_FOURCC_NV12, dst_format = VA_FOURCC_NV12;
    unsigned rotation = VA_ROTATION_NONE;
    bool tight = false;
};
static void run_case(Case c) {
    Allocation input(c.sw, c.sh, c.src_format, c.tight), reference(c.dw, c.dh, c.dst_format),
        destination(c.dw, c.dh, c.dst_format, c.tight);
    bool wide = c.src_format == VA_FOURCC_P010 || c.dst_format == VA_FOURCC_P010;
    auto &sl = input.desc.layers[0];
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memset(input.data, 0xa5, input.desc.objects[0].size);
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned y = 0; y < (p ? c.sh / 2 : c.sh); ++y)
            for (unsigned x = 0; x < c.sw; ++x)
                if (c.src_format == VA_FOURCC_P010)
                    reinterpret_cast<uint16_t *>(input.data + sl.offset[p] + y * sl.pitch[p])[x] =
                        ((x * 37 + y * 53 + (p ? 257 : 0)) & 1023) << 6;
                else
                    input.data[sl.offset[p] + y * sl.pitch[p] + x] =
                        wide ? (x * 37 + y * 53 + p * 71) & 255
                        : p  ? (x & 1 ? 150 + (x / 2 + y) % 80 : 20 + (x / 2 + y) % 80)
                             : 16 + (x / 8 + y / 4) % 220;
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    VARectangle region{static_cast<int16_t>(c.crop ? 16 : 0), static_cast<int16_t>(c.crop ? 2 : 0),
                       static_cast<uint16_t>(c.crop ? 608 : c.sw),
                       static_cast<uint16_t>(c.crop ? 360 : c.sh)};
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    unsigned scaled_width = (c.rotation & 1) ? c.dh : c.dw;
    unsigned scaled_height = (c.rotation & 1) ? c.dw : c.dh;
    bool enlarging = region.width < scaled_width || region.height < scaled_height;
    std::vector<int> row(region.width);
    for (unsigned p = 0; p < 2; ++p) {
        int channels = p ? 2 : 1;
        const auto *src = input.data + sl.offset[p] + (region.y / channels) * sl.pitch[p] +
                          region.x * (c.src_format == VA_FOURCC_P010 ? 2 : 1);
        auto *dst = reference.data + reference.desc.layers[0].offset[p];
        for (unsigned y = 0; y < scaled_height / channels; ++y) {
            auto fy = make_filter(region.height / channels, scaled_height / channels, y);
            for (unsigned x = 0; x < region.width; ++x) {
                int sum = 0;
                for (int k = 0; k < fy.tap_count; ++k) {
                    const auto *line = src + fy.indices[k] * sl.pitch[p];
                    int sample = c.src_format == VA_FOURCC_P010
                                     ? reinterpret_cast<const uint16_t *>(line)[x] >> 6
                                     : line[x] << (wide ? 2 : 0);
                    sum += sample * fy.weights[k];
                }
                if (wide)
                    row[x] = (sum + (1 << (fy.shift - 5))) >> (fy.shift - 4);
                else
                    row[x] = enlarging ? (sum + (1 << (fy.shift - 7))) >> (fy.shift - 6)
                                       : round_clip_u8(sum, fy.shift);
            }
            for (unsigned x = 0; x < scaled_width / channels; ++x) {
                auto fx = make_filter(region.width / channels, scaled_width / channels, x);
                for (int ch = 0; ch < channels; ++ch) {
                    int sum = 0;
                    for (int k = 0; k < fx.tap_count; ++k)
                        sum += row[fx.indices[k] * channels + ch] * fx.weights[k];
                    unsigned ox = x, oy = y;
                    if (c.rotation == VA_ROTATION_90) {
                        ox = scaled_height / channels - 1 - y;
                        oy = x;
                    } else if (c.rotation == VA_ROTATION_180) {
                        ox = scaled_width / channels - 1 - x;
                        oy = scaled_height / channels - 1 - y;
                    } else if (c.rotation == VA_ROTATION_270) {
                        ox = y;
                        oy = scaled_width / channels - 1 - x;
                    }
                    auto *line = dst + oy * reference.desc.layers[0].pitch[p];
                    int shift = fx.shift + (wide ? 4 : enlarging ? 6 : 0);
                    if (c.dst_format == VA_FOURCC_P010)
                        reinterpret_cast<uint16_t *>(line)[ox * channels + ch] =
                            clamp_int((sum + (1 << (shift - 1))) >> shift, 0, 1023) << 6;
                    else
                        line[ox * channels + ch] = round_clip_u8(sum, shift + (wide ? 2 : 0));
                }
            }
        }
    }
    sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    require(drm >= 0, "open DRM");
    VADisplay display = vaGetDisplayDRM(drm);
    int major, minor;
    va(vaInitialize(display, &major, &minor));
    VAConfigAttrib rt{VAConfigAttribRTFormat, 0};
    va(vaGetConfigAttributes(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1));
    require(rt.value == (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_RGB32),
            "VPP render formats");
    VAConfigID config;
    VAContextID context;
    rt.value = c.dst_format == VA_FOURCC_P010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    va(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &config));
    unsigned attribute_count = 0;
    va(vaQuerySurfaceAttributes(display, config, nullptr, &attribute_count));
    std::vector<VASurfaceAttrib> attributes(attribute_count);
    va(vaQuerySurfaceAttributes(display, config, attributes.data(), &attribute_count));
    unsigned formats = 0;
    for (const auto &a : attributes)
        if (a.type == VASurfaceAttribPixelFormat)
            formats |= a.value.value.i == VA_FOURCC_NV12   ? 1
                       : a.value.value.i == VA_FOURCC_P010 ? 2
                                                           : 0;
    require(formats == 3, "VPP surface formats");
    va(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context));
    VAProcFilterType filters[VAProcFilterCount]{};
    unsigned n = VAProcFilterCount;
    va(vaQueryVideoProcFilters(display, context, filters, &n));
    require(n == 0 || (n == 1 && filters[0] == VAProcFilterHighDynamicRangeToneMapping),
            "available VPP filters");
    VAProcPipelineCaps caps{};
    va(vaQueryVideoProcPipelineCaps(display, context, nullptr, 0, &caps));
    require(caps.num_input_pixel_formats == 4 && caps.num_output_pixel_formats == 2,
            "VPP input/output format count");
    require(caps.input_pixel_format && caps.input_pixel_format[0] == VA_FOURCC_NV12 &&
                caps.input_pixel_format[1] == VA_FOURCC_P010 &&
                caps.output_pixel_format[1] == VA_FOURCC_P010,
            "NV12/P010 capability values");
    require(caps.rotation_flags == ((1 << VA_ROTATION_NONE) | (1 << VA_ROTATION_90) |
                                    (1 << VA_ROTATION_180) | (1 << VA_ROTATION_270)),
            "rotation capabilities");
    auto in = import(display, input.desc);
    // Test composed and separate-layer PRIME2 imports.
    auto separate = destination.desc;
    separate.num_layers = 2;
    separate.layers[0].drm_format = c.dst_format == VA_FOURCC_P010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
    separate.layers[0].num_planes = 1;
    separate.layers[1].drm_format =
        c.dst_format == VA_FOURCC_P010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
    separate.layers[1].num_planes = 1;
    separate.layers[1].pitch[0] = destination.desc.layers[0].pitch[1];
    separate.layers[1].offset[0] = destination.desc.layers[0].offset[1];
    VASurfaceID outputs[3];
    outputs[0] = import(display, separate);
    va(vaCreateSurfaces(
        display, c.dst_format == VA_FOURCC_P010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420,
        c.dw, c.dh, &outputs[1], 2, nullptr, 0));
    int initial_maps = maps, initial_unmaps = unmaps;
    unsigned initial_imports = imports;
    saw_input = false;
    for (unsigned kind = 0; kind < 3; ++kind) {
        VADRMPRIMESurfaceDescriptor exported{};
        // kind 0: external heap; kind 1: pre-exported MSM; kind 2: lazy VPP heap.
        if (kind != 2) {
            va(vaExportSurfaceHandle(
                display, outputs[kind], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported));
            output_fd = exported.objects[0].fd;
        } else
            output_fd = -1;
        input_fd = input.desc.objects[0].fd;
        direct_calls = 0;
        saw_output = false;
        for (int repeat = 0; repeat < 3; ++repeat) {
            VAProcPipelineParameterBuffer params{};
            params.surface = in;
            params.rotation_state = c.rotation;
            params.surface_region = &region;
            VABufferID buffer;
            va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(params),
                              1, &params, &buffer));
            va(vaBeginPicture(display, context, outputs[kind]));
            va(vaRenderPicture(display, context, &buffer, 1));
            va(vaDestroyBuffer(display, buffer)); // Parameters must already be snapshotted.
            va(vaEndPicture(display, context));
            va(vaSyncSurface(display, outputs[kind]));
            if (kind == 2 && repeat == 0) {
                va(vaExportSurfaceHandle(
                    display, outputs[kind], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported));
                output_fd = exported.objects[0].fd;
            }
        }
        if (gpu) {
            require(imports - initial_imports == kind + 2, "persistent Vulkan imports");
            require(saw_input && (kind == 2 || saw_output), "original DMA-BUFs reach Vulkan");
            require(maps == initial_maps && !direct_calls, "Vulkan processing does not call DSP");
        } else {
            require(maps - initial_maps == int(kind) + 2,
                    "one persistent map per source/destination");
            require(unmaps == initial_unmaps, "maps survive across frames");
            require(direct_calls == (kind == 2 ? 2 : 3),
                    "both original DMA-BUFs must reach successful FastRPC calls");
        }
        if (benchmark && kind == 0) {
            VAProcPipelineParameterBuffer params{};
            params.surface = in;
            params.surface_region = &region;
            params.rotation_state = c.rotation;
            VABufferID buffer;
            va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(params),
                              1, &params, &buffer));
            auto submit = [&] {
                va(vaBeginPicture(display, context, outputs[0]));
                va(vaRenderPicture(display, context, &buffer, 1));
                va(vaEndPicture(display, context));
                va(vaSyncSurface(display, outputs[0]));
            };
            auto warm = std::chrono::steady_clock::now();
            do {
                submit();
            } while (
                std::chrono::duration<double>(std::chrono::steady_clock::now() - warm).count() < 1);
            std::vector<double> times;
            for (int i = 0; i < 100; ++i) {
                auto begin = std::chrono::steady_clock::now();
                submit();
                times.push_back(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count());
            }
            std::sort(times.begin(), times.end());
            std::printf(
                "BENCH %s %s->%s %ux%u->%ux%u crop=%d rotation=%u median=%.3f ms p95=%.3f ms\n",
                gpu ? "vulkan" : "cdsp", c.src_format == VA_FOURCC_P010 ? "P010" : "NV12",
                c.dst_format == VA_FOURCC_P010 ? "P010" : "NV12", c.sw, c.sh, c.dw, c.dh, c.crop,
                c.rotation * 90, times[50], times[95]);
            va(vaDestroyBuffer(display, buffer));
        }
        VAImage image{};
        va(vaDeriveImage(display, outputs[kind], &image));
        void *mapped;
        va(vaMapBuffer(display, image.buf, &mapped));
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        unsigned maxdiff[2]{};
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned y = 0; y < (p ? c.dh / 2 : c.dh); ++y)
                for (unsigned x = 0; x < c.dw; ++x) {
                    const auto *actual =
                        static_cast<uint8_t *>(mapped) + image.offsets[p] + y * image.pitches[p];
                    const auto *expected = reference.data + reference.desc.layers[0].offset[p] +
                                           y * reference.desc.layers[0].pitch[p];
                    int a = c.dst_format == VA_FOURCC_P010
                                ? reinterpret_cast<const uint16_t *>(actual)[x] >> 6
                                : actual[x];
                    int b = c.dst_format == VA_FOURCC_P010
                                ? reinterpret_cast<const uint16_t *>(expected)[x] >> 6
                                : expected[x];
                    if (c.dst_format == VA_FOURCC_P010)
                        require(!(reinterpret_cast<const uint16_t *>(actual)[x] & 63),
                                "P010 low bits zero");
                    maxdiff[p] = std::max(maxdiff[p], unsigned(std::abs(a - b)));
                }
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        va(vaUnmapBuffer(display, image.buf));
        va(vaDestroyImage(display, image.image_id));
        std::printf(
            "%s -> %s %ux%u -> %ux%u crop=%d rotation=%u output=%u RPC=%d maxdiff Y=%u UV=%u\n",
            c.src_format == VA_FOURCC_P010 ? "P010" : "NV12",
            c.dst_format == VA_FOURCC_P010 ? "P010" : "NV12", c.sw, c.sh, c.dw, c.dh, c.crop,
            c.rotation * 90, kind, direct_calls, maxdiff[0], maxdiff[1]);
        require(maxdiff[0] <= unsigned(wide ? 0 : 1) && maxdiff[1] <= unsigned(wide ? 0 : 1),
                "VPP matches CPU reference within rounding tolerance");
        close(exported.objects[0].fd);
    }
    // Unsupported operations must fail before a DSP invocation.
    VAProcPipelineParameterBuffer bad{};
    bad.surface = in;
    bad.rotation_state = VA_ROTATION_270 + 1;
    VABufferID buffer;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(bad), 1, &bad,
                      &buffer));
    va(vaBeginPicture(display, context, outputs[0]));
    require(vaRenderPicture(display, context, &buffer, 1) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "reject invalid rotation");
    require(vaEndPicture(display, context) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "reject missing parameters");
    va(vaDestroyBuffer(display, buffer));
    auto alias = import(display, input.desc);
    bad = {};
    bad.surface = in;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(bad), 1, &bad,
                      &buffer));
    va(vaBeginPicture(display, context, alias));
    va(vaRenderPicture(display, context, &buffer, 1));
    int before = direct_calls;
    require(vaEndPicture(display, context) == VA_STATUS_ERROR_INVALID_SURFACE,
            "reject DMA-BUF alias");
    require(direct_calls == before, "alias must not reach DSP");
    va(vaDestroyBuffer(display, buffer));
    va(vaDestroySurfaces(display, &alias, 1));
    va(vaDestroyContext(display, context));
    if (!gpu)
        require(unmaps - initial_unmaps == 4, "last context retires all persistent maps");
    // Surfaces outlive the last context, then get mapped into a new session.
    va(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context));
    VAProcPipelineParameterBuffer again{};
    again.surface = in;
    again.surface_region = &region;
    again.rotation_state = c.rotation;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(again), 1, &again,
                      &buffer));
    va(vaBeginPicture(display, context, outputs[0]));
    va(vaRenderPicture(display, context, &buffer, 1));
    va(vaEndPicture(display, context));
    va(vaDestroyBuffer(display, buffer));
    if (gpu)
        require(imports - initial_imports == 4, "imports outlive their VA context");
    else
        require(maps - initial_maps == 6, "retained buffers remap into a new context");
    va(vaDestroyContext(display, context));
    if (!gpu)
        require(unmaps - initial_unmaps == 6, "recreated context cleans up maps");
    va(vaDestroySurfaces(display, &in, 1));
    va(vaDestroySurfaces(display, outputs, 3));
    va(vaDestroyConfig(display, config));
    va(vaTerminate(display));
    close(drm);
}
#ifndef VPP_NO_CDSP
static cdsp_vpp_buffer *import_test_buffer(size_t size) {
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    require(heap >= 0, "heap open");
    dma_heap_allocation_data a{};
    a.len = (size + 4095) & ~4095;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    require(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) == 0, "heap allocate");
    close(heap);
    cdsp_vpp_buffer *b = nullptr;
    // Deliberately import a size that is not a page multiple.
    require(cdsp_vpp_import(a.fd, size, &b) == 0, "import");
    close(a.fd);
    return b;
}
static std::vector<uint8_t> pixels(const cdsp_vpp_frame &f, size_t size, int value) {
    std::vector<uint8_t> data(size, 0xa5);
    for (unsigned p = 0; p < 2; p++)
        for (unsigned y = 0; y < f.height / (p ? 2 : 1); y++)
            for (unsigned x = 0; x < f.width; x++)
                data[(p ? f.uv_offset : f.y_offset) + y * f.stride + x] =
                    p ? ((x & 1) ? 255 - value : value / 2) : value;
    return data;
}
static void check_buffer_lifetime() {
    struct Profile {
        uint64_t before = UINT64_C(0x123456789abcdef0);
        uint64_t counters[CDSP_VPP_PROFILE_COUNTERS]{};
        uint64_t after = UINT64_C(0x123456789abcdef0);
    } profile;
    constexpr size_t source_size = 4081, dest_size = 12271;
    // Import before opening the first context: mapping must remain lazy.
    auto *s = import_test_buffer(source_size), *d = import_test_buffer(dest_size);
    cdsp_vpp_frame src{s, 80, 18, 104, 16, 16 + 104 * 20};
    cdsp_vpp_frame dst{d, 160, 36, 184, 16, 16 + 184 * 38};
    auto *sp = (uint8_t *)cdsp_vpp_buffer_map(s);
    auto *dp = (uint8_t *)cdsp_vpp_buffer_map(d);
    constexpr int cycles = 2, frames = 3;
    for (int cycle = 0; cycle < cycles; cycle++) {
        cdsp_vpp_context *ctx = nullptr;
        require(cdsp_vpp_create(&ctx, CDSP_VPP_MAX_WORKERS, 1) == 0, "context create");
        require(cdsp_vpp_profile(ctx, 1, profile.counters) == 0, "enable profiling");
        for (int frame = 0; frame < frames; frame++) {
            int value = (cycle * 31 + frame * 17) & 255;
            auto input = pixels(src, source_size, value);
            auto expected = pixels(dst, dest_size, value);
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            memcpy(sp, input.data(), source_size);
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            memset(dp, 0xa5, dest_size);
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            uint64_t us;
            require(cdsp_vpp_scale(ctx, &src, nullptr, &dst, &us) == 0, "persistent scale");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            require(memcmp(dp, expected.data(), dest_size) == 0, "pixels and all padding");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            require(memcmp(sp, input.data(), source_size) == 0, "source unchanged");
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            // A second import of the same source gets its own mapping lifetime.
            cdsp_vpp_buffer *alias = nullptr;
            require(cdsp_vpp_import(cdsp_vpp_buffer_fd(s), source_size, &alias) == 0,
                    "reimport source");
            auto aliased_src = src;
            aliased_src.buffer = alias;
            require(cdsp_vpp_scale(ctx, &aliased_src, nullptr, &dst, &us) == 0,
                    "scale reimported source");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            require(memcmp(dp, expected.data(), dest_size) == 0, "reimported pixels");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            cdsp_vpp_release(alias);
        }
        require(cdsp_vpp_profile(ctx, 0, profile.counters) == 0, "read all worker counters");
        require(profile.before == UINT64_C(0x123456789abcdef0) &&
                    profile.after == UINT64_C(0x123456789abcdef0),
                "profiling buffer guards");
        // Retain the original mapped buffers across context destruction/recreation.
        cdsp_vpp_destroy(ctx);
    }
    cdsp_vpp_context *a = nullptr, *b = nullptr;
    require(cdsp_vpp_create(&a, CDSP_VPP_MAX_WORKERS, 1) == 0 && cdsp_vpp_create(&b, 2, 1) == 0,
            "two live contexts");
    uint64_t us;
    require(cdsp_vpp_scale(a, &src, nullptr, &dst, &us) == 0, "first shared context");
    require(cdsp_vpp_scale(b, &src, nullptr, &dst, &us) == 0, "second shared context");
    cdsp_vpp_destroy(a);
    require(cdsp_vpp_scale(b, &src, nullptr, &dst, &us) == 0, "surviving context retains mappings");
    auto expected = pixels(dst, dest_size, ((cycles - 1) * 31 + (frames - 1) * 17) & 255);
    sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    require(memcmp(dp, expected.data(), dest_size) == 0, "surviving context pixels");
    sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    cdsp_vpp_destroy(b);
    cdsp_vpp_release(d);
    cdsp_vpp_release(s);
    puts("PASS: buffer lifetime, changing pixels, padding, duplicate imports and profiling");
}

// Keep enough wide rotation strips alive to exceed the test board's VTCM.
// Every context must remain usable when subsequent allocations fall back to DDR.
static void check_rotation_contexts() {
    constexpr size_t source_size = 1920 * 64 * 3 / 2, dest_size = 256 * 3840 * 3 / 2;
    auto *source = import_test_buffer(source_size), *target = import_test_buffer(dest_size);
    cdsp_vpp_frame src{source, 1920, 64, 1920, 0, 1920 * 64};
    cdsp_vpp_frame dst{target, 128, 3840, 256, 0, 256 * 3840};
    cdsp_vpp_context *contexts[5]{};
    for (auto &ctx : contexts)
        require(!cdsp_vpp_create(&ctx, CDSP_VPP_MAX_WORKERS, 1), "rotation context create");
    for (int pass = 0; pass < 2; pass++) {
        int value = 64 + pass * 32;
        auto input = pixels(src, source_size, value);
        sync(cdsp_vpp_buffer_fd(source), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        memcpy(cdsp_vpp_buffer_map(source), input.data(), source_size);
        sync(cdsp_vpp_buffer_fd(source), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        for (auto *ctx : contexts) {
            if (!ctx)
                continue;
            uint64_t us;
            require(!cdsp_vpp_transform(ctx, &src, CDSP_VPP_NV12, nullptr, &dst, CDSP_VPP_P010,
                                        CDSP_VPP_ROTATE_90, &us),
                    "rotation with multiple live contexts");
            sync(cdsp_vpp_buffer_fd(target), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            auto *out = static_cast<const uint16_t *>(cdsp_vpp_buffer_map(target));
            for (unsigned y = 0; y < dst.height * 3 / 2; y++)
                for (unsigned x = 0; x < dst.width; x++) {
                    unsigned expected = y < dst.height ? value : (x & 1) ? 255 - value : value / 2;
                    require(out[y * dst.width + x] == expected * 256, "rotation context pixels");
                }
            sync(cdsp_vpp_buffer_fd(target), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        }
        // Releasing one context must not invalidate any surviving workspace.
        cdsp_vpp_destroy(contexts[0]);
        contexts[0] = nullptr;
    }
    for (auto *ctx : contexts)
        cdsp_vpp_destroy(ctx);
    cdsp_vpp_release(target);
    cdsp_vpp_release(source);
    puts("PASS: rotation scratch with five live contexts and surviving allocations");
}

static void check_transform_boundaries() {
    constexpr size_t size = 16384;
    auto *source = import_test_buffer(size), *target = import_test_buffer(size);
    cdsp_vpp_frame src{source, 288, 8, 600, 16, 16 + 600 * 8};
    cdsp_vpp_frame dst{target, 272, 4, 584, 32, 32 + 584 * 6};
    cdsp_vpp_crop crop{2, 2, 272, 4};
    for (int workers : {0, 1, CDSP_VPP_MAX_WORKERS}) {
        cdsp_vpp_context *ctx = nullptr;
        require(!cdsp_vpp_create(&ctx, workers, 1), "conversion context");
        for (int sf : {CDSP_VPP_NV12, CDSP_VPP_P010})
            for (int df : {CDSP_VPP_NV12, CDSP_VPP_P010})
                for (int rotation : {CDSP_VPP_ROTATE_NONE, CDSP_VPP_ROTATE_90, CDSP_VPP_ROTATE_180,
                                     CDSP_VPP_ROTATE_270}) {
                    dst = (rotation & 1) ? cdsp_vpp_frame{target, 4, 272, 24, 32, 32 + 24 * 274}
                                         : cdsp_vpp_frame{target, 272, 4, 584, 32, 32 + 584 * 6};
                    std::vector<uint8_t> input(size, 0xa5), expected(size, 0xa5);
                    for (unsigned plane = 0; plane < 2; plane++) {
                        unsigned channels = plane ? 2 : 1;
                        for (unsigned y = 0; y < src.height / channels; y++)
                            for (unsigned x = 0; x < src.width; x++) {
                                unsigned value =
                                    (x + y * src.width + plane * 71) & (sf ? 1023 : 255);
                                auto *row = input.data() + (plane ? src.uv_offset : src.y_offset) +
                                            y * src.stride;
                                if (sf)
                                    reinterpret_cast<uint16_t *>(row)[x] = value << 6;
                                else
                                    row[x] = value;
                            }
                        for (unsigned y = 0; y < crop.height / channels; y++)
                            for (unsigned x = 0; x < crop.width; x++) {
                                const auto *row = input.data() +
                                                  (plane ? src.uv_offset : src.y_offset) +
                                                  (y + crop.y / channels) * src.stride;
                                unsigned value =
                                    sf ? reinterpret_cast<const uint16_t *>(row)[x + crop.x] >> 6
                                       : row[x + crop.x] * 4;
                                unsigned px = x / channels, py = y;
                                if (rotation == CDSP_VPP_ROTATE_90) {
                                    px = crop.height / channels - y - 1;
                                    py = x / channels;
                                } else if (rotation == CDSP_VPP_ROTATE_180) {
                                    px = crop.width / channels - x / channels - 1;
                                    py = crop.height / channels - y - 1;
                                } else if (rotation == CDSP_VPP_ROTATE_270) {
                                    px = y;
                                    py = crop.width / channels - x / channels - 1;
                                }
                                auto *out = expected.data() +
                                            (plane ? dst.uv_offset : dst.y_offset) +
                                            py * dst.stride;
                                unsigned component = px * channels + x % channels;
                                if (df)
                                    reinterpret_cast<uint16_t *>(out)[component] = value << 6;
                                else
                                    out[component] = std::min((value + 2) / 4, 255u);
                            }
                    }
                    sync(cdsp_vpp_buffer_fd(source), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
                    memcpy(cdsp_vpp_buffer_map(source), input.data(), size);
                    sync(cdsp_vpp_buffer_fd(source), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                    sync(cdsp_vpp_buffer_fd(target), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
                    memset(cdsp_vpp_buffer_map(target), 0xa5, size);
                    sync(cdsp_vpp_buffer_fd(target), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                    uint64_t us;
                    require(!cdsp_vpp_transform(ctx, &src, sf, &crop, &dst, df, rotation, &us),
                            "conversion, rotation and format switch");
                    sync(cdsp_vpp_buffer_fd(target), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
                    require(!memcmp(cdsp_vpp_buffer_map(target), expected.data(), size),
                            "conversion values and all padding");
                    sync(cdsp_vpp_buffer_fd(target), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                }
        uint64_t us;
        for (int rotation : {-1, CDSP_VPP_ROTATE_270 + 1})
            require(cdsp_vpp_transform(ctx, &src, CDSP_VPP_NV12, &crop, &dst, CDSP_VPP_NV12,
                                       rotation, &us) != 0,
                    "reject rotation");
        require(cdsp_vpp_process(ctx, &src, 2, &crop, &dst, CDSP_VPP_NV12, &us) != 0,
                "reject format");
        auto bad = src;
        bad.stride = src.width;
        require(cdsp_vpp_process(ctx, &bad, CDSP_VPP_P010, &crop, &dst, CDSP_VPP_NV12, &us) != 0,
                "reject short P010 stride");
        bad = src;
        bad.y_offset++;
        require(cdsp_vpp_process(ctx, &bad, CDSP_VPP_P010, &crop, &dst, CDSP_VPP_NV12, &us) != 0,
                "reject unaligned P010");
        cdsp_vpp_destroy(ctx);
    }
    cdsp_vpp_release(target);
    cdsp_vpp_release(source);
    puts("PASS: conversion/rotation, format switches, padding and invalid layouts (scalar/1 "
         "HVX/all HVX)");
}

#endif
int main(int argc, char **argv) {
#ifdef VPP_NO_CDSP
    gpu = true;
#endif
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bench"))
            benchmark = true;
        else {
            require(!std::strcmp(argv[i], "cdsp") || !std::strcmp(argv[i], "vulkan"),
                    "backend argument");
            gpu = !std::strcmp(argv[i], "vulkan");
        }
    }
#ifdef VPP_NO_CDSP
    require(gpu, "CDSP tests were disabled at build time");
#endif
    setenv("IRIS_VAAPI_VPP", gpu ? "vulkan" : "cdsp", 1);
    std::vector<Case> cases = {{1920, 1080, 3840, 2160, false},
                               {1920, 1080, 1280, 720, false},
                               {1280, 720, 1920, 1080, false},
                               {3840, 2160, 1920, 1080, false},
                               {640, 360, 1280, 720, false},
                               {640, 360, 640, 360, false},
                               {640, 384, 320, 180, true},
                               {16, 2, 32, 4, false},
                               {32, 4, 16, 2, false}};
    for (unsigned src : {VA_FOURCC_NV12, VA_FOURCC_P010})
        for (unsigned dst : {VA_FOURCC_NV12, VA_FOURCC_P010}) {
            if (src == VA_FOURCC_NV12 && dst == VA_FOURCC_NV12)
                continue;
            for (Case c : {Case{1920, 1080, 1920, 1080, false}, Case{1920, 1080, 3840, 2160, false},
                           Case{3840, 2160, 1920, 1080, false}, Case{1920, 1080, 1280, 720, false},
                           Case{64, 18, 128, 18, false}, Case{128, 36, 128, 18, false},
                           Case{640, 384, 608, 360, true}, Case{640, 384, 320, 180, true},
                           Case{80, 18, 160, 36, false}, Case{80, 18, 112, 24, false},
                           Case{640, 400, 32, 20, false}, Case{16, 2, 16, 2, false},
                           Case{32, 4, 16, 2, false}}) {
                c.src_format = src;
                c.dst_format = dst;
                cases.push_back(c);
            }
        }
    for (unsigned sf : {VA_FOURCC_NV12, VA_FOURCC_P010})
        for (unsigned df : {VA_FOURCC_NV12, VA_FOURCC_P010})
            for (unsigned rotation : {VA_ROTATION_90, VA_ROTATION_180, VA_ROTATION_270})
                for (Case c : {Case{162, 94, 162, 94, false}, Case{80, 18, 160, 36, false},
                               Case{320, 180, 160, 90, false}, Case{82, 18, 114, 26, false},
                               Case{640, 384, 320, 180, true}, Case{2, 2, 2, 2, false},
                               Case{2, 2, 6, 4, false}}) {
                    c.src_format = sf;
                    c.dst_format = df;
                    c.rotation = rotation;
                    if (rotation & 1)
                        std::swap(c.dw, c.dh);
                    cases.push_back(c);
                }
    for (unsigned format : {VA_FOURCC_NV12, VA_FOURCC_P010})
        for (unsigned rotation : {VA_ROTATION_90, VA_ROTATION_180, VA_ROTATION_270}) {
            Case c{1920, 1080, 1920, 1080, false, format, format, rotation};
            if (rotation & 1)
                std::swap(c.dw, c.dh);
            cases.push_back(c);
        }
    cases.push_back(
        {1080, 1920, 1920, 1080, false, VA_FOURCC_NV12, VA_FOURCC_NV12, VA_ROTATION_90});
    // Page-ending buffers expose masked vector accesses even with no active
    // bytes beyond the boundary. Include the portrait 2x transcode geometry.
    for (unsigned sf : {VA_FOURCC_NV12, VA_FOURCC_P010})
        for (unsigned df : {VA_FOURCC_NV12, VA_FOURCC_P010}) {
            // Noninteger enlargement: overlapping vector tails, repeated phases,
            // two-vector gathers near 1:1, and an unchanged horizontal axis.
            for (Case c : {Case{66, 10, 88, 14, false}, Case{130, 10, 174, 14, false},
                           Case{288, 162, 384, 216, false}, Case{256, 16, 258, 18, false},
                           Case{130, 10, 130, 14, false}}) {
                c.src_format = sf;
                c.dst_format = df;
                c.tight = true;
                cases.push_back(c);
            }
            // Four-tap area rows also use the unrolled P010 vertical kernel.
            cases.push_back({256, 24, 64, 6, false, sf, df});
            cases.push_back({720, 1280, 360, 640, false, sf, df});
            cases.push_back({64, 128, 96, 192, false, sf, df, VA_ROTATION_NONE, true});
            cases.push_back({128, 256, 64, 128, false, sf, df, VA_ROTATION_NONE, true});
            cases.push_back({64, 128, 128, 64, false, sf, df, VA_ROTATION_90, true});
            cases.push_back({64, 128, 128, 64, false, sf, df, VA_ROTATION_270, true});
        }
    if (benchmark) {
        cases = {{1920, 1080, 3840, 2160, false},
                 {3840, 2160, 1920, 1080, false},
                 {1920, 1080, 1280, 720, false},
                 {1280, 720, 1920, 1080, false},
                 {3840, 2160, 3840, 2160, false},
                 {3840, 2160, 3840, 2160, false, VA_FOURCC_NV12, VA_FOURCC_P010},
                 {3840, 2160, 3840, 2160, false, VA_FOURCC_P010, VA_FOURCC_NV12},
                 {1920, 1080, 3840, 2160, false, VA_FOURCC_P010, VA_FOURCC_P010},
                 {3840, 2160, 2160, 3840, false, VA_FOURCC_NV12, VA_FOURCC_NV12, VA_ROTATION_90},
                 {3840, 2160, 2160, 3840, false, VA_FOURCC_P010, VA_FOURCC_P010, VA_ROTATION_90},
                 {1920, 1080, 2160, 3840, false, VA_FOURCC_NV12, VA_FOURCC_P010, VA_ROTATION_270},
                 {640, 384, 608, 360, true},
                 {640, 400, 32, 20, false}};
    }
    // FastRPC owns threads and process-global state. Never fork a process
    // that has already initialized them: each case starts in a fresh child.
    for (auto c : cases) {
        pid_t child = fork();
        require(child >= 0, "fork test case");
        if (!child) {
            run_case(c);
            std::exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && status == 0, "test case child");
    }
#ifndef VPP_NO_CDSP
    if (!gpu && !benchmark) {
        check_buffer_lifetime();
        check_transform_boundaries();
        check_rotation_contexts();
    }
#endif
    std::puts("PASS");
}
