// brightness_filter — Adjusts image brightness using OpenCV.
//
// Demonstrates:
//   - Frame input and output
//   - OpenCV integration (cv_helpers.h for format conversion)
//   - Pool-allocated output with fallback to internal buffer
//   - Input validation
//   - .streaming() and .frame_alloc() capabilities

#include <speculor/plugin_helpers.h>
#include <cv_helpers.h>
#include <opencv2/imgproc.hpp>

struct BrightnessState {
    spc::HostServices host;
    float brightness = 0.0f;   // -100 to +100
    float contrast = 1.0f;     // 0.0 to 3.0
    cv::Mat result;
    SpcFrame fallback_frame{};
};

SPC_PLUGIN(BrightnessState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("brightness_filter", "Brightness / Contrast", "Examples")
        .author("Speculor").version("1.0.0")
        .description("Adjusts image brightness and contrast using OpenCV")
        .input("frame", "Frame", SPC_DATA_FRAME, 32, SPC_CONSUME_LATEST)
        .output("frame", "Frame", SPC_DATA_FRAME)
        .float_param("brightness", "Brightness", -100.0f, 100.0f, 0.0f, 1.0f)
        .float_param("contrast", "Contrast", 0.0f, 3.0f, 1.0f, 0.01f)
        .streaming().frame_alloc()
        .build()
)

SPC_PLUGIN_AUTO_PARAMS(BrightnessState,
    SPC_BIND_FLOAT(BrightnessState, "brightness", brightness),
    SPC_BIND_FLOAT(BrightnessState, "contrast", contrast)
)

static int start(SpcPluginInstance* inst) {
    SPC_LOG_INFO(&state(inst)->host.cached_log, "brightness filter started");
    return 0;
}

static int stop(SpcPluginInstance*) { return 0; }

static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    if (input_count < 1 || output_count < 1) return -1;

    // validate input
    const SpcFrame* in = spc::input_frame(inputs, input_count, 0);
    if (!in) return -1;

    int cv_type = spc::cv_type_for_format(in->format);
    if (cv_type < 0) return -1;

    // wrap input frame as cv::Mat (zero-copy)
    cv::Mat src(static_cast<int>(in->height), static_cast<int>(in->width),
                cv_type, in->data, static_cast<size_t>(in->stride));

    // passthrough if no adjustment
    if (s->brightness == 0.0f && s->contrast == 1.0f) {
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = const_cast<SpcFrame*>(in);
        return 0;
    }

    // try pool-allocated output
    SpcFrame* out = s->host.acquire_frame(0, in->width, in->height, in->format);
    if (out) {
        cv::Mat dst(static_cast<int>(out->height), static_cast<int>(out->width),
                    cv_type, out->data, static_cast<size_t>(out->stride));
        src.convertTo(dst, -1, s->contrast, s->brightness);
        out->frame_number = in->frame_number;
        out->timestamp_us = in->timestamp_us;
        spc::set_frame_output(outputs[0], out);
    } else {
        // fallback to internal buffer
        src.convertTo(s->result, -1, s->contrast, s->brightness);
        spc::mat_to_frame(s->result, &s->fallback_frame, in->format,
                          in->frame_number, in->timestamp_us);
        spc::set_frame_output(outputs[0], &s->fallback_frame);
    }
    return 0;
}

SPC_DECLARE_PLUGIN_SOURCE(get_descriptor, create_instance, destroy_instance,
                          set_parameter, get_parameter, process, start, stop)
