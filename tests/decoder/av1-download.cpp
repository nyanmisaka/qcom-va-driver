// SPDX-License-Identifier: MIT
// Used by av1.py to download VA frames with their actual plane strides.
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc != 3)
        return 2;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(argv[1], &error);
    if (!pipeline || error) {
        fprintf(stderr, "pipeline: %s\n", error ? error->message : "null");
        return 1;
    }
    auto *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "output"));
    if (!sink)
        return 2;
    auto *bus = gst_element_get_bus(pipeline);
    FILE *file = fopen(argv[2], "wb");
    if (!file)
        return 1;
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    int count = 0, result = 0;
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        auto *sample = gst_app_sink_try_pull_sample(sink, GST_SECOND);
        if (!sample) {
            auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (message) {
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                fprintf(stderr, "GStreamer error: %s / %s\n", error->message, debug ? debug : "");
                gst_message_unref(message);
                result = 1;
                break;
            }
            if (gst_app_sink_is_eos(sink))
                break;
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10)) {
                fprintf(stderr, "timeout\n");
                result = 1;
                break;
            }
            continue;
        }
        last = std::chrono::steady_clock::now();
        GstVideoInfo info{};
        GstVideoFrame frame{};
        auto *buffer = gst_sample_get_buffer(sample);
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
            !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
            fprintf(stderr, "video frame mapping failed\n");
            result = 1;
            gst_sample_unref(sample);
            break;
        }
        auto format = GST_VIDEO_FRAME_FORMAT(&frame);
        unsigned w = GST_VIDEO_FRAME_WIDTH(&frame), h = GST_VIDEO_FRAME_HEIGHT(&frame);
        if (format != GST_VIDEO_FORMAT_NV12 && format != GST_VIDEO_FORMAT_P010_10LE) {
            fprintf(stderr, "unexpected format\n");
            result = 1;
            gst_video_frame_unmap(&frame);
            gst_sample_unref(sample);
            break;
        }
        unsigned bytes = w * (format == GST_VIDEO_FORMAT_NV12 ? 1 : 2);
        printf("frame=%d pts=%llu size=%zu %ux%u strides=%d,%d offsets=%zu,%zu flags=%x\n", count,
               (unsigned long long)GST_BUFFER_PTS(buffer), gst_buffer_get_size(buffer), w, h,
               frame.info.stride[0], frame.info.stride[1], frame.info.offset[0],
               frame.info.offset[1], GST_BUFFER_FLAGS(buffer));
        for (unsigned plane = 0; plane < 2; ++plane) {
            auto *data =
                static_cast<const unsigned char *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, plane));
            for (unsigned y = 0; y < (plane ? h / 2 : h); ++y)
                if (fwrite(data + size_t(y) * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane), 1, bytes,
                           file) != bytes)
                    result = 1;
        }
        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);
        ++count;
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    fclose(file);
    printf("RESULT frames=%d exit=%d\n", count, result);
    return result;
}
