// frame_stats — Computes statistics from input frames.
//
// Demonstrates:
//   - Multiple output ports (frame passthrough + scalar statistics)
//   - Reading frame data with OpenCV
//   - Enum parameters
//   - Logging

#include <speculor/plugin_helpers.h>
#include <cv_helpers.h>
#include <opencv2/core.hpp>

struct FrameStatsState {
    spc::HostServices host;
    int32_t channel = 0;  // 0=all, 1=R, 2=G, 3=B
};

SPC_PLUGIN(FrameStatsState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("frame_stats", "Frame Statistics", "Examples")
        .author("Speculor").version("1.0.0")
        .description("Computes mean and standard deviation of input frames")
        .input("frame", "Frame", SPC_DATA_FRAME, 32, SPC_CONSUME_LATEST)
        .output("frame", "Frame", SPC_DATA_FRAME)
        .output("mean", "Mean", SPC_DATA_SCALAR)
        .output("stddev", "Std Dev", SPC_DATA_SCALAR)
        .enum_param("channel", "Channel", {"All", "Red", "Green", "Blue"}, 0)
        .streaming()
        .build()
)

SPC_PLUGIN_AUTO_PARAMS(FrameStatsState,
    SPC_BIND_ENUM(FrameStatsState, "channel", channel)
)

static int start(SpcPluginInstance* inst) {
    SPC_LOG_INFO(&state(inst)->host.cached_log, "frame stats started");
    return 0;
}

static int stop(SpcPluginInstance*) { return 0; }

static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    if (input_count < 1 || output_count < 3) return -1;

    const SpcFrame* in = spc::input_frame(inputs, input_count, 0);
    if (!in) return -1;

    int cv_type = spc::cv_type_for_format(in->format);
    if (cv_type < 0) return -1;

    cv::Mat src(static_cast<int>(in->height), static_cast<int>(in->width),
                cv_type, in->data, static_cast<size_t>(in->stride));

    cv::Scalar mean_val, stddev_val;
    cv::meanStdDev(src, mean_val, stddev_val);

    // select channel
    int ch = s->channel;
    double mean = (ch == 0) ? (mean_val[0] + mean_val[1] + mean_val[2]) / 3.0
                            : mean_val[ch - 1];
    double stddev = (ch == 0) ? (stddev_val[0] + stddev_val[1] + stddev_val[2]) / 3.0
                              : stddev_val[ch - 1];

    // output 0: passthrough frame
    outputs[0].type = SPC_DATA_FRAME;
    outputs[0].frame = const_cast<SpcFrame*>(in);

    // output 1: mean
    spc::set_scalar_output(outputs[1], static_cast<float>(mean));

    // output 2: standard deviation
    spc::set_scalar_output(outputs[2], static_cast<float>(stddev));

    return 0;
}

SPC_DECLARE_PLUGIN_SOURCE(get_descriptor, create_instance, destroy_instance,
                          set_parameter, get_parameter, process, start, stop)
