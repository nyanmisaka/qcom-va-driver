// SPDX-License-Identifier: MIT
// Build: c++ -O2 -std=c++17 -rdynamic tests/vpp/tonemap.cpp -o build/vpp-tonemap \
//   $(pkg-config --cflags --libs libva libva-drm libdrm vulkan) -ldl
// Run with --geometry to check combined processing with the selected VPP backend.
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <drm_fourcc.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

#include <vulkan/vulkan.h>
#include <array>
#include <cmath>
#include <chrono>
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

static unsigned imports = 0;
static int expected_input = -1, expected_output = -1;
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
    for (auto next = static_cast<const VkBaseInStructure *>(info->pNext); next;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
            auto fd = reinterpret_cast<const VkImportMemoryFdInfoKHR *>(next);
            if (fd->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
                ++imports;
                saw_input |= same_buffer(fd->fd, expected_input);
                saw_output |= same_buffer(fd->fd, expected_output);
            }
        }
    }
    return real(device, info, callbacks, memory);
}

using RGB = std::array<double, 3>;
static double pq(double nits) {
    double t = std::pow(nits / 10000.0, 0.1593017578125);
    return std::pow((0.8359375 + 18.8515625 * t) / (1 + 18.6875 * t), 78.84375);
}
static double linear(double code) {
    double t = std::pow(std::clamp(code, 0.0, 1.0), 1.0 / 78.84375);
    return 10000 * std::pow(std::max(t - 0.8359375, 0.0) / (18.8515625 - 18.6875 * t),
                            1.0 / 0.1593017578125);
}
static double dot(RGB a, RGB b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static std::array<int, 3> reference(unsigned y, unsigned u, unsigned v, double peak) {
    double yy = (double(y) - 64) / 876, uu = (double(u) - 512) / 896, vv = (double(v) - 512) / 896;
    double r = yy + 1.4746 * vv, b = yy + 1.8814 * uu;
    RGB rgb{linear(r), linear((yy - .2627 * r - .0593 * b) / .678), linear(b)};
    rgb = {dot(rgb, {1.660491, -.587641, -.072850}), dot(rgb, {-.124550, 1.132900, -.008349}),
           dot(rgb, {-.018151, -.100579, 1.118730})};
    double luma = std::max(dot(rgb, {.2126, .7152, .0722}), 0.0);
    double negative = std::max(-*std::min_element(rgb.begin(), rgb.end()), 0.0);
    for (auto &channel : rgb)
        channel = (channel + negative) * luma / std::max(luma + negative, 1e-20);
    double maximum = *std::max_element(rgb.begin(), rgb.end()), mapped = maximum;
    if (peak > 100) {
        double source_pq = pq(peak), destination = pq(100) / source_pq;
        double knee = 1.5 * destination - .5, x = std::min(pq(maximum) / source_pq, 1.0);
        double t = std::clamp((x - knee) / (1 - knee), 0.0, 1.0);
        // Independent Hermite basis form of the shoulder used by the shader.
        double shoulder = (2 * t * t * t - 3 * t * t + 1) * knee +
                          (t * t * t - 2 * t * t + t) * (1 - knee) +
                          (-2 * t * t * t + 3 * t * t) * destination;
        mapped = linear((x < knee ? x : shoulder) * source_pq);
    }
    for (auto &channel : rgb)
        channel = std::pow(std::clamp(channel * mapped / std::max(maximum, 1e-20) / 100, 0.0, 1.0),
                           1.0 / 2.4);
    double y709 = dot(rgb, {.2126, .7152, .0722});
    return {int(std::nearbyint(64 + 876 * y709)),
            int(std::nearbyint(512 + (rgb[2] - y709) * 896 / 1.8556)),
            int(std::nearbyint(512 + (rgb[0] - y709) * 896 / 1.5748))};
}
struct Session {
    int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    VADisplay display;
    VAConfigID config;
    VAContextID context;
    Session() {
        require(render >= 0, "open render node");
        display = vaGetDisplayDRM(render);
        int major, minor;
        va(vaInitialize(display, &major, &minor));
        va(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &config));
        va(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context));
        VAProcFilterType filters[VAProcFilterCount];
        unsigned count = VAProcFilterCount;
        va(vaQueryVideoProcFilters(display, context, filters, &count));
        require(count == 1 && filters[0] == VAProcFilterHighDynamicRangeToneMapping,
                "HDR filter advertised");
        count = 0;
        require(vaQueryVideoProcFilters(display, context, nullptr, &count) ==
                        VA_STATUS_ERROR_MAX_NUM_EXCEEDED &&
                    count == 1,
                "filter capacity check");
        VAProcFilterCapHighDynamicRange caps{};
        count = 1;
        va(vaQueryVideoProcFilterCaps(display, context, VAProcFilterHighDynamicRangeToneMapping,
                                      &caps, &count));
        require(count == 1 && caps.metadata_type == VAProcHighDynamicRangeMetadataHDR10 &&
                    caps.caps_flag == VA_TONE_MAPPING_HDR_TO_SDR,
                "HDR10 to SDR capability");
    }
    ~Session() {
        va(vaDestroyContext(display, context));
        va(vaDestroyConfig(display, config));
        va(vaTerminate(display));
        close(render);
    }
    void submit(VASurfaceID target, VABufferID pipeline) {
        va(vaBeginPicture(display, context, target));
        va(vaRenderPicture(display, context, &pipeline, 1));
        va(vaEndPicture(display, context));
    }
};
static VABufferID pipeline(Session &s, VASurfaceID source, VABufferID *filter,
                           VARectangle *crop = nullptr, unsigned rotation = VA_ROTATION_NONE) {
    VAProcPipelineParameterBuffer params{};
    params.surface = source;
    params.surface_region = crop;
    params.filters = filter;
    params.num_filters = filter ? 1 : 0;
    params.rotation_state = rotation;
    if (filter) {
        params.surface_color_standard = VAProcColorStandardBT2020;
        params.output_color_standard = VAProcColorStandardBT709;
    }
    VABufferID id;
    va(vaCreateBuffer(s.display, s.context, VAProcPipelineParameterBufferType, sizeof(params), 1,
                      &params, &id));
    return id;
}
static void fill(Allocation &input, uint32_t random = 42) {
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memset(input.data, 0xa5, input.desc.objects[0].size);
    const auto &layout = input.desc.layers[0];
    for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned y = 0; y < input.desc.height / (plane ? 2 : 1); ++y)
            for (unsigned x = 0; x < input.desc.width; ++x) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                reinterpret_cast<uint16_t *>(input.data + layout.offset[plane] +
                                             y * layout.pitch[plane])[x] = (random & 1023) << 6;
            }
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}
static void check_pixels(Allocation &input, Allocation &output, VARectangle crop, double peak) {
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    sync(output.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    auto &src = input.desc.layers[0];
    auto &dst = output.desc.layers[0];
    unsigned max_error = 0;
    uint64_t total_error = 0;
    std::vector<bool> active(output.desc.objects[0].size);
    auto compare = [&](unsigned offset, int expected) {
        unsigned error = std::abs(int(output.data[offset]) - std::clamp(expected, 0, 255));
        if (error > 1 && max_error <= 1)
            std::fprintf(stderr, "pixel offset=%u actual=%u expected=%d\n", offset,
                         output.data[offset], expected);
        max_error = std::max(max_error, error);
        total_error += error;
        active[offset] = true;
    };
    for (unsigned y = 0; y < crop.height; y += 2)
        for (unsigned x = 0; x < crop.width; x += 2) {
            auto uv = reinterpret_cast<uint16_t *>(input.data + src.offset[1] +
                                                   (y + crop.y) / 2 * src.pitch[1]) +
                      x + crop.x;
            int sum_u = 0, sum_v = 0;
            for (unsigned row = 0; row < 2; ++row)
                for (unsigned column = 0; column < 2; ++column) {
                    auto yy = reinterpret_cast<uint16_t *>(input.data + src.offset[0] +
                                                           (y + crop.y + row) * src.pitch[0]);
                    auto expected =
                        reference(yy[x + crop.x + column] >> 6, uv[0] >> 6, uv[1] >> 6, peak);
                    compare(dst.offset[0] + (y + row) * dst.pitch[0] + x + column,
                            (expected[0] + 2) / 4);
                    sum_u += expected[1];
                    sum_v += expected[2];
                }
            compare(dst.offset[1] + y / 2 * dst.pitch[1] + x, (sum_u + 8) / 16);
            compare(dst.offset[1] + y / 2 * dst.pitch[1] + x + 1, (sum_v + 8) / 16);
        }
    for (unsigned i = 0; i < active.size(); ++i)
        require(active[i] || output.data[i] == 0xa5, "output padding preserved");
    std::printf("%ux%u peak=%.0f max_error=%u MAE=%.6f\n", crop.width, crop.height, peak, max_error,
                double(total_error) / (crop.width * crop.height * 1.5));
    require(max_error <= 1, "FP32 within one NV12 code of double reference");
    sync(output.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}
static void run_case(Session &s, unsigned width, unsigned height, unsigned peak,
                     bool cropped = false, bool tight = false, bool geometry = false,
                     unsigned mastering_peak = 0) {
    VARectangle crop{int16_t(cropped ? 2 : 0), int16_t(cropped ? 2 : 0),
                     uint16_t(width - (cropped ? 2 : 0)), uint16_t(height - (cropped ? 4 : 0))};
    Allocation input(width, height, VA_FOURCC_P010, tight);
    Allocation output(crop.width, crop.height, VA_FOURCC_NV12, tight);
    fill(input);
    sync(output.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memset(output.data, 0xa5, output.desc.objects[0].size);
    sync(output.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    VASurfaceID surfaces[] = {import(s.display, input.desc), import(s.display, output.desc)};
    VAHdrMetaDataHDR10 metadata{};
    metadata.max_content_light_level = peak;
    metadata.max_display_mastering_luminance = mastering_peak * 10000;
    VAProcFilterParameterBufferHDRToneMapping hdr{};
    hdr.type = VAProcFilterHighDynamicRangeToneMapping;
    hdr.data.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
    hdr.data.metadata = &metadata;
    hdr.data.metadata_size = sizeof(metadata);
    VABufferID filter;
    va(vaCreateBuffer(s.display, s.context, VAProcFilterParameterBufferType, sizeof(hdr), 1, &hdr,
                      &filter));
    VAProcPipelineCaps caps{};
    va(vaQueryVideoProcPipelineCaps(s.display, s.context, &filter, 1, &caps));
    require(caps.num_input_pixel_formats == 1 && caps.input_pixel_format[0] == VA_FOURCC_P010 &&
                caps.num_output_pixel_formats == 1 && caps.output_pixel_format[0] == VA_FOURCC_NV12,
            "HDR formats");
    auto params = pipeline(s, surfaces[0], &filter, &crop);
    if (width == 2) {
        auto reject = [&](VAStatus expected) {
            va(vaBeginPicture(s.display, s.context, surfaces[1]));
            require(vaRenderPicture(s.display, s.context, &params, 1) == expected,
                    "unsupported HDR request rejected");
            require(vaEndPicture(s.display, s.context) != VA_STATUS_SUCCESS,
                    "failed render does not submit work");
        };
        void *mapped;
        va(vaMapBuffer(s.display, filter, &mapped));
        auto *filter_data = static_cast<VAProcFilterParameterBufferHDRToneMapping *>(mapped);
        filter_data->data.metadata = nullptr;
        va(vaUnmapBuffer(s.display, filter));
        va(vaQueryVideoProcPipelineCaps(s.display, s.context, &filter, 1, &caps));
        reject(VA_STATUS_ERROR_INVALID_PARAMETER);
        va(vaMapBuffer(s.display, filter, &mapped));
        std::memcpy(mapped, &hdr, sizeof(hdr));
        static_cast<VAProcFilterParameterBufferHDRToneMapping *>(mapped)->data.metadata_size--;
        va(vaUnmapBuffer(s.display, filter));
        reject(VA_STATUS_ERROR_INVALID_PARAMETER);
        va(vaMapBuffer(s.display, filter, &mapped));
        std::memcpy(mapped, &hdr, sizeof(hdr));
        va(vaUnmapBuffer(s.display, filter));
        va(vaMapBuffer(s.display, params, &mapped));
        static_cast<VAProcPipelineParameterBuffer *>(mapped)->input_color_properties.color_range =
            VA_SOURCE_RANGE_FULL;
        va(vaUnmapBuffer(s.display, params));
        reject(VA_STATUS_ERROR_UNIMPLEMENTED);
        va(vaMapBuffer(s.display, params, &mapped));
        static_cast<VAProcPipelineParameterBuffer *>(mapped)->input_color_properties.color_range =
            VA_SOURCE_RANGE_UNKNOWN;
        va(vaUnmapBuffer(s.display, params));
    }
    expected_input = input.desc.objects[0].fd;
    expected_output = output.desc.objects[0].fd;
    saw_input = saw_output = false;
    unsigned before = imports;
    s.submit(surfaces[1], params);
    require(imports == before + 2 && saw_input && saw_output,
            "Vulkan imported original input/output DMA-BUFs");
    unsigned source_peak = peak ? peak : mastering_peak ? mastering_peak : 1000;
    check_pixels(input, output, crop, std::clamp(source_peak, 100u, 10000u));
    if (width == 130 && tight) {
        fill(input, 99);
        s.submit(surfaces[1], params);
        check_pixels(input, output, crop, source_peak);
    }
    // Metadata may change between frames, and is consumed during RenderPicture.
    metadata.max_content_light_level = 4000;
    va(vaBeginPicture(s.display, s.context, surfaces[1]));
    va(vaRenderPicture(s.display, s.context, &params, 1));
    metadata.max_content_light_level = 100;
    va(vaEndPicture(s.display, s.context));
    check_pixels(input, output, crop, 4000);
    metadata.max_content_light_level = peak;
    std::vector<double> times;
    for (unsigned i = 0; i < 110; ++i) {
        auto start = std::chrono::steady_clock::now();
        s.submit(surfaces[1], params);
        auto end = std::chrono::steady_clock::now();
        if (i >= 10)
            times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    require(imports == before + 2, "persistent Vulkan imports");
    std::sort(times.begin(), times.end());
    std::printf("VAAPI %ux%u median=%.3f ms p95=%.3f ms\n", crop.width, crop.height, times[50],
                times[95]);
    VADRMPRIMESurfaceDescriptor exported{};
    va(vaExportSurfaceHandle(s.display, surfaces[1], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                             VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                             &exported));
    require(exported.num_objects == 1 && same_buffer(exported.objects[0].fd, expected_output),
            "output DMA-BUF identity");
    close(exported.objects[0].fd);
    if (geometry) {
        Allocation combined(crop.height / 2, crop.width / 2),
            separate(crop.height / 2, crop.width / 2);
        VASurfaceID outputs[] = {import(s.display, combined.desc),
                                 import(s.display, separate.desc)};
        auto first = pipeline(s, surfaces[0], &filter, &crop, VA_ROTATION_90);
        auto second = pipeline(s, surfaces[1], nullptr, nullptr, VA_ROTATION_90);
        for (unsigned frame_peak : {100u, 1000u, 4000u}) {
            metadata.max_content_light_level = frame_peak;
            s.submit(surfaces[1], params);
            s.submit(outputs[0], first);
            s.submit(outputs[1], second);
            sync(combined.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            sync(separate.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            for (unsigned plane = 0; plane < 2; ++plane)
                for (unsigned y = 0; y < combined.desc.height / (plane ? 2 : 1); ++y)
                    require(!std::memcmp(combined.data + combined.desc.layers[0].offset[plane] +
                                             y * combined.desc.layers[0].pitch[plane],
                                         separate.data + separate.desc.layers[0].offset[plane] +
                                             y * separate.desc.layers[0].pitch[plane],
                                         combined.desc.width),
                            "combined HDR/scale/rotation matches separate operations");
            sync(combined.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            sync(separate.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        }
        metadata.max_content_light_level = peak;
        va(vaDestroyBuffer(s.display, first));
        va(vaDestroyBuffer(s.display, second));
        va(vaDestroySurfaces(s.display, outputs, 2));
        std::puts("combined HDR + scale + rotation PASS");
    }
    va(vaDestroyBuffer(s.display, params));
    va(vaDestroyBuffer(s.display, filter));
    // Imports must survive destruction of the context that first used them.
    va(vaDestroyContext(s.display, s.context));
    va(vaCreateContext(s.display, s.config, 0, 0, 0, nullptr, 0, &s.context));
    va(vaCreateBuffer(s.display, s.context, VAProcFilterParameterBufferType, sizeof(hdr), 1, &hdr,
                      &filter));
    params = pipeline(s, surfaces[0], &filter, &crop);
    unsigned retained = imports;
    s.submit(surfaces[1], params);
    require(imports == retained, "imports retained across VPP contexts");
    va(vaDestroyBuffer(s.display, params));
    va(vaDestroyBuffer(s.display, filter));
    va(vaDestroySurfaces(s.display, surfaces, 2));
}
int main(int argc, char **argv) {
    bool geometry = argc == 2 && !std::strcmp(argv[1], "--geometry");
    Session session;
    run_case(session, 2, 2, 100, false, true);
    run_case(session, 130, 6, 1000, false, true);
    run_case(session, 132, 10, 0, true);
    run_case(session, 130, 6, 0, false, false, false, 4000);
    run_case(session, 256, 128, 10000, false, false, geometry);
    run_case(session, 1920, 1080, 1000);
    run_case(session, 3840, 2160, 4000);
    std::puts("PASS");
}
