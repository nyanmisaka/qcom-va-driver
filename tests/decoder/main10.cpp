// SPDX-License-Identifier: MIT
// Board test: c++ -std=c++17 main10.cpp -lva -lva-drm -o main10
#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

static void require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
static void expect(VAStatus status, VAStatus wanted = VA_STATUS_SUCCESS) {
    if (status != wanted) {
        std::fprintf(stderr, "Expected %s, got %s\n", vaErrorStr(wanted), vaErrorStr(status));
        std::exit(1);
    }
}
static void av1_rejected_pictures(VADisplay display) {
    VAConfigID config;
    VAContextID context;
    VASurfaceID surfaces[2];
    expect(vaCreateConfig(display, VAProfileAV1Profile0, VAEntrypointVLD, nullptr, 0, &config));
    expect(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 128, 128, surfaces, 2, nullptr, 0));
    expect(vaCreateContext(display, config, 128, 128, VA_PROGRESSIVE, nullptr, 0, &context));
    VADecPictureParameterBufferAV1 base{};
    base.current_frame = surfaces[0];
    base.frame_width_minus1 = base.frame_height_minus1 = 127;
    base.seq_info_fields.fields.subsampling_x = base.seq_info_fields.fields.subsampling_y = 1;
    base.pic_info_fields.bits.show_frame = 1;
    base.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
    base.primary_ref_frame = 7;
    base.base_qindex = 20;
    base.tile_cols = base.tile_rows = 1;
    base.matrix_coefficients = 2;
    std::fill(std::begin(base.ref_frame_map), std::end(base.ref_frame_map), VA_INVALID_SURFACE);
    // All cases must be rejected before the dummy tile reaches the firmware.
    auto reject = [&](VADecPictureParameterBufferAV1 picture, VAStatus status,
                      unsigned tile_row = 0) {
        VASliceParameterBufferAV1 slice{};
        slice.slice_data_size = 1;
        slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        slice.tile_row = tile_row;
        unsigned char byte = 0;
        VABufferID buffers[3];
        expect(vaCreateBuffer(display, context, VAPictureParameterBufferType, sizeof(picture), 1,
                              &picture, &buffers[0]));
        expect(vaCreateBuffer(display, context, VASliceParameterBufferType, sizeof(slice), 1,
                              &slice, &buffers[1]));
        expect(vaCreateBuffer(display, context, VASliceDataBufferType, 1, 1, &byte, &buffers[2]));
        // Cover both parameter-first clients and Chromium's data-first order.
        unsigned order[] = {0, 1, 2};
        do {
            VABufferID submission[] = {buffers[order[0]], buffers[order[1]], buffers[order[2]]};
            expect(vaBeginPicture(display, context, surfaces[0]));
            expect(vaRenderPicture(display, context, submission, 3));
            expect(vaEndPicture(display, context), status);
        } while (std::next_permutation(std::begin(order), std::end(order)));
        for (auto buffer : buffers)
            expect(vaDestroyBuffer(display, buffer));
    };
    auto picture = base;
    picture.film_grain_info.film_grain_info_fields.bits.apply_grain = 1;
    reject(picture, VA_STATUS_ERROR_UNIMPLEMENTED);
    picture = base;
    picture.pic_info_fields.bits.large_scale_tile = 1;
    reject(picture, VA_STATUS_ERROR_UNIMPLEMENTED);
    picture = base;
    picture.bit_depth_idx = 1;
    reject(picture, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    picture = base;
    picture.current_frame = surfaces[1];
    reject(picture, VA_STATUS_ERROR_INVALID_SURFACE);
    picture = base;
    picture.pic_info_fields.bits.use_superres = 1;
    picture.superres_scale_denominator = 8;
    reject(picture, VA_STATUS_ERROR_INVALID_BUFFER);
    reject(base, VA_STATUS_ERROR_INVALID_BUFFER, 1);
    picture = base;
    picture.pic_info_fields.bits.frame_type = 1;
    reject(picture, VA_STATUS_ERROR_INVALID_BUFFER);
    expect(vaDestroyContext(display, context));
    expect(vaDestroySurfaces(display, surfaces, 2));
    expect(vaDestroyConfig(display, config));
}
static void av1_slice_buffer_pairing(VADisplay display) {
    VAConfigID config;
    VAContextID context;
    VASurfaceID surface;
    expect(vaCreateConfig(display, VAProfileAV1Profile0, VAEntrypointVLD, nullptr, 0, &config));
    expect(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 128, 128, &surface, 1, nullptr, 0));
    expect(vaCreateContext(display, config, 128, 128, VA_PROGRESSIVE, nullptr, 0, &context));
    VASliceParameterBufferAV1 slice{};
    slice.slice_data_size = 1;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    unsigned char bytes[2]{};
    VABufferID buffers[4];
    // The second offset is outside the first buffer. Moving all parameters
    // ahead of all data would pair it with the wrong buffer and fail.
    for (unsigned i = 0; i < 2; ++i) {
        slice.slice_data_offset = i;
        expect(vaCreateBuffer(display, context, VASliceParameterBufferType, sizeof(slice), 1,
                              &slice, &buffers[2 * i]));
        expect(vaCreateBuffer(display, context, VASliceDataBufferType, i + 1, 1, bytes,
                              &buffers[2 * i + 1]));
    }
    for (unsigned batch_size : {1u, 3u, 4u}) {
        expect(vaBeginPicture(display, context, surface));
        for (unsigned i = 0; i < 4; i += batch_size)
            expect(vaRenderPicture(display, context, buffers + i, std::min(batch_size, 4 - i)));
        // No picture parameters: reject before sending dummy data to firmware.
        expect(vaEndPicture(display, context), VA_STATUS_ERROR_INVALID_BUFFER);
    }
    for (auto buffer : buffers)
        expect(vaDestroyBuffer(display, buffer));
    expect(vaDestroyContext(display, context));
    expect(vaDestroySurfaces(display, &surface, 1));
    expect(vaDestroyConfig(display, config));
}
int main() {
    int fd = open("/dev/dri/renderD128", O_RDWR);
    require(fd >= 0, "open DRM device");
    auto display = vaGetDisplayDRM(fd);
    int major, minor;
    expect(vaInitialize(display, &major, &minor));
    std::vector<VAProfile> advertised(vaMaxNumProfiles(display));
    int profile_count = 0;
    expect(vaQueryConfigProfiles(display, advertised.data(), &profile_count));
    advertised.resize(profile_count);
    bool av1 =
        std::find(advertised.begin(), advertised.end(), VAProfileAV1Profile0) != advertised.end();
    std::vector<VAProfile> mixed_profiles{VAProfileHEVCMain10};
    if (av1)
        mixed_profiles.push_back(VAProfileAV1Profile0);
    for (VAProfile requested_profile : mixed_profiles) {
        constexpr unsigned formats = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
        VAConfigAttrib attr{VAConfigAttribRTFormat, 0};
        expect(vaGetConfigAttributes(display, requested_profile, VAEntrypointVLD, &attr, 1));
        require(attr.value == formats, "profile must advertise both 8-bit and 10-bit");
        VAConfigID default_config;
        expect(vaCreateConfig(display, requested_profile, VAEntrypointVLD, nullptr, 0,
                              &default_config));
        VAProfile profile;
        VAEntrypoint entrypoint;
        std::vector<VAConfigAttrib> config_attrs(vaMaxNumConfigAttributes(display));
        int config_count = 0;
        expect(vaQueryConfigAttributes(display, default_config, &profile, &entrypoint,
                                       config_attrs.data(), &config_count));
        require(profile == requested_profile && entrypoint == VAEntrypointVLD &&
                    config_count == 1 && config_attrs[0].type == VAConfigAttribRTFormat &&
                    config_attrs[0].value == formats,
                "default mixed-depth config formats");
        expect(vaDestroyConfig(display, default_config));
        const unsigned requests[] = {VA_RT_FORMAT_YUV420, VA_RT_FORMAT_YUV420_10,       formats, 0,
                                     VA_RT_FORMAT_YUV422, formats | VA_RT_FORMAT_YUV422};
        for (unsigned requested : requests) {
            attr.value = requested;
            VAConfigID config;
            bool valid = requested && !(requested & ~formats);
            expect(vaCreateConfig(display, requested_profile, VAEntrypointVLD, &attr, 1, &config),
                   valid ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
            if (!valid)
                continue;
            unsigned n = 0;
            expect(vaQuerySurfaceAttributes(display, config, nullptr, &n));
            std::vector<VASurfaceAttrib> attrs(n);
            unsigned too_small = n - 1;
            expect(vaQuerySurfaceAttributes(display, config, attrs.data(), &too_small),
                   VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
            require(too_small == n, "surface attribute count");
            expect(vaQuerySurfaceAttributes(display, config, attrs.data(), &n));
            bool nv12 = false, p010 = false;
            for (const auto &a : attrs) {
                if (a.type == VASurfaceAttribPixelFormat) {
                    nv12 |= a.value.value.i == VA_FOURCC_NV12;
                    p010 |= a.value.value.i == VA_FOURCC_P010;
                }
            }
            require(nv12 && p010, "mixed-depth surface formats");
            for (unsigned format : {VA_RT_FORMAT_YUV420, VA_RT_FORMAT_YUV420_10}) {
                VASurfaceID surface, other;
                expect(vaCreateSurfaces(display, format, 128, 128, &surface, 1, nullptr, 0));
                expect(
                    vaCreateSurfaces(display, format ^ formats, 128, 128, &other, 1, nullptr, 0));
                VAContextID context;
                // Chromium can create the context before allocating its surfaces.
                expect(vaCreateContext(display, config, 128, 128, VA_PROGRESSIVE, nullptr, 0,
                                       &context));
                expect(vaBeginPicture(display, context, surface));
                expect(vaEndPicture(display, context), VA_STATUS_ERROR_INVALID_BUFFER);
                expect(vaBeginPicture(display, context, other),
                       VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                expect(vaDestroyContext(display, context));
                expect(vaDestroySurfaces(display, &surface, 1));
                expect(vaDestroySurfaces(display, &other, 1));
            }
            expect(vaDestroyConfig(display, config));
        }
    }
    VAConfigAttrib features{VAConfigAttribDecAV1Features, 0};
    expect(vaGetConfigAttributes(display, VAProfileAV1Profile0, VAEntrypointVLD, &features, 1),
           av1 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    if (av1) {
        require(features.value == 0, "AV1 large scale tiles must not be advertised");
        features.value = 1;
        VAConfigID config;
        expect(
            vaCreateConfig(display, VAProfileAV1Profile0, VAEntrypointVLD, &features, 1, &config),
            VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
        expect(vaCreateConfig(display, VAProfileAV1Profile0, VAEntrypointEncSlice, nullptr, 0,
                              &config),
               VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        av1_rejected_pictures(display);
        av1_slice_buffer_pairing(display);
    }
    VAConfigAttrib attr{VAConfigAttribRTFormat, 0};
    expect(vaGetConfigAttributes(display, VAProfileVP9Profile2, VAEntrypointVLD, &attr, 1));
    require(attr.value == VA_RT_FORMAT_YUV420_10, "VP9 profile 2 remains 10-bit");
    expect(vaTerminate(display));
    close(fd);
    std::puts("Main10/AV1 capability/config/surface tests passed");
}
