// Draws the bounding boxes of a TABLE input onto the frames of a FRAME input,
// with OpenCV. The table is read through its field_offsets, required fields
// and optional ones alike.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>
#include <cv_helpers.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <format>
#include <new>
#include <string>
#include <vector>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_COLOR,
    P_THICKNESS,
    P_SHOW_INFO,
    P_RESIZE_ENABLE,
    P_RESIZE_WIDTH,
    P_RESIZE_HEIGHT,
};

// the input ports, in declaration order
enum Input : uint32_t { IN_BOXES, IN_IMAGE };

// boxes_in field indices, in the order the input declares them: the required
// fields, then the optional ones
enum {
    F_X = 0, F_Y, F_W, F_H, F_CONFIDENCE,
    F_AREA, F_MEAN_INTENSITY, F_LABEL_NAME, F_CLASS_NAME,
    IN_FIELD_COUNT
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("bbox_display", "BB Display", "Renderers/Overlays")
        .author("Speculor").version("0.1.0")
        .help("Draws bounding boxes on an image frame")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"tracking", "image"})
        .input_table("boxes_in", "Boxes", {
            {"x", SPC_FIELD_FLOAT}, {"y", SPC_FIELD_FLOAT},
            {"w", SPC_FIELD_FLOAT}, {"h", SPC_FIELD_FLOAT},
            {"confidence", SPC_FIELD_FLOAT},
        }, 4, SPC_CONSUME_LATEST)
        .optional_fields({
            {"area", SPC_FIELD_FLOAT},
            {"mean_intensity", SPC_FIELD_FLOAT},
            {"label_name", SPC_FIELD_STRING32},
            {"class_name", SPC_FIELD_STRING32},
        })
        .input("image_in", "Image", SPC_DATA_FRAME)
        .output("image_out", "Image Out", SPC_DATA_FRAME)
        .color_param("color", "Color", spc::color_pack(0, 255, 0))
            .param_help("Bounding box outline color")
        .int_param("thickness", "Thickness", 1, 10, 2, 1)
            .param_help("Bounding box line thickness in pixels")
        .bool_param("show_info", "Show Info", true)
            .param_help("Show confidence and class label next to each box")
        .bool_param("resize_enable", "Resize", false, "Output Resolution")
            .param_help("Resize the output image to a specific resolution")
        .int_param("resize_width", "Width", 32, 7680, 1920, 1, "Output Resolution")
            .param_help("Output image width in pixels")
        .int_param("resize_height", "Height", 32, 4320, 1080, 1, "Output Resolution")
            .param_help("Output image height in pixels")
)

struct Params {
    uint32_t color_rgba;
    int32_t thickness;
    int32_t show_info;
    int32_t resize_enable;
    int32_t resize_width;
    int32_t resize_height;
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.color_rgba    = values[P_COLOR].color;   // COLOR: 0xRRGGBBAA
    p.thickness     = values[P_THICKNESS].i;
    p.show_info     = values[P_SHOW_INFO].i;
    p.resize_enable = values[P_RESIZE_ENABLE].i;
    p.resize_width  = values[P_RESIZE_WIDTH].i;
    p.resize_height = values[P_RESIZE_HEIGHT].i;
    return p;
}

struct BboxDisplayState {
    SpcHost* host = nullptr;   // valid until stop() returns
    Params params{};

    // A frame of the plugin's own, for when the host lends none or the output
    // is not the copied input. The engine copies it, so it only has to stay
    // valid until process() returns.
    std::vector<uint8_t> buffer;
    SpcFrame output_frame{};
    cv::Mat converted_rgb; // keeps RGB conversion alive across output
    cv::Mat resized_mat;   // keeps resized frame alive across output
};

static BboxDisplayState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<BboxDisplayState*>(instance);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) BboxDisplayState{};
    if (!s) return nullptr;
    s->host = host;
    s->params = read_params(values);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    delete state(instance);
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->params = read_params(values);
}

// Which parameters the values leave disabled: a function of the values alone,
// called with no instance, on any thread. `states` has one entry per parameter.
static void param_rules(const SpcValue* values, uint32_t* states)
{
    if (values[P_RESIZE_ENABLE].i == 0) {
        states[P_RESIZE_WIDTH]  |= SPC_PARAM_STATE_DISABLED;
        states[P_RESIZE_HEIGHT] |= SPC_PARAM_STATE_DISABLED;
    }
}

// acquire pool frame or fallback buffer, copy input, return cv::Mat wrapping dst
static cv::Mat acquire_and_copy_frame(BboxDisplayState* s, const SpcFrame* in,
                                      int cv_type, SpcFrame** out_pool_frame)
{
    SpcHost* host = s->host;
    *out_pool_frame = host->frame ? host->frame(host, 0, in->width, in->height, in->format)
                                  : nullptr;

    uint8_t* dst_ptr;
    uint32_t dst_stride;
    if (*out_pool_frame) {
        dst_ptr = (*out_pool_frame)->data;
        dst_stride = (*out_pool_frame)->stride;
    } else {
        size_t needed = static_cast<size_t>(in->stride) * in->height;
        if (s->buffer.size() < needed)
            s->buffer.resize(needed);
        dst_ptr = s->buffer.data();
        dst_stride = in->stride;
    }

    spc::copy_frame_rows(in->data, in->stride, dst_ptr, dst_stride, in->height);

    return {static_cast<int>(in->height), static_cast<int>(in->width),
            cv_type, dst_ptr, static_cast<size_t>(dst_stride)};
}

// set output data (pool path or fallback path)
static void set_frame_output(BboxDisplayState* s, const SpcFrame* in,
                             SpcFrame* pool_frame, SpcData* output)
{
    if (pool_frame) {
        pool_frame->timestamp_ns = in->timestamp_ns;
        pool_frame->sequence = in->sequence;
        output->type = SPC_DATA_FRAME;
        output->frame = pool_frame;
    } else {
        s->output_frame = *in;
        s->output_frame.data = s->buffer.data();
        output->type = SPC_DATA_FRAME;
        output->frame = &s->output_frame;
    }
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const Params& p = s->params;
    const SpcData* inputs = step->inputs;
    SpcData* outputs = step->outputs;

    if (inputs[IN_IMAGE].type != SPC_DATA_FRAME || !inputs[IN_IMAGE].frame) return SPC_SKIP;

    const SpcFrame* in = inputs[IN_IMAGE].frame;
    int cv_type = spc::cv_type_for_format(in->format);
    if (cv_type < 0) return SPC_FAILED;

    SpcFrame* out_frame = nullptr;
    cv::Mat dst = acquire_and_copy_frame(s, in, cv_type, &out_frame);

    // convert grayscale to RGB so colored overlays are visible
    bool was_grayscale = false;
    if (dst.channels() == 1)
    {
        cv::Mat tmp;
        dst.convertTo(tmp, CV_8U);
        cv::cvtColor(tmp, s->converted_rgb, cv::COLOR_GRAY2RGB);
        dst = s->converted_rgb;
        was_grayscale = true;
    }

    // draw boxes from the first input
    if (inputs[IN_BOXES].type == SPC_DATA_TABLE && inputs[IN_BOXES].table) {
        const SpcTable* tbl = inputs[IN_BOXES].table;

        // Where each declared field is in this table's records, which are
        // laid out by the producer; an optional field the producer does not
        // send is SPC_NO_FIELD. spc_table_get_* (table_helpers.h) read a field
        // at its offset.
        const uint32_t* off = tbl->field_offsets;
        // prefer label_name (smoothed by track_class_enricher) over class_name
        // (raw detector / mode-of-history vote) — same convention as track_display
        const uint32_t off_name = off[F_LABEL_NAME] != SPC_NO_FIELD
            ? off[F_LABEL_NAME] : off[F_CLASS_NAME];

        uint8_t cr, cg, cb, ca;
        spc::color_unpack(p.color_rgba, cr, cg, cb, ca);
        const cv::Scalar color(cr, cg, cb); // RGB
        auto fw = static_cast<float>(in->width);
        auto fh = static_cast<float>(in->height);

        for (uint32_t i = 0; i < tbl->record_count; ++i) {
            float bx = spc_table_get_float(tbl, i, off[F_X]);
            float by = spc_table_get_float(tbl, i, off[F_Y]);
            float bw = spc_table_get_float(tbl, i, off[F_W]);
            float bh = spc_table_get_float(tbl, i, off[F_H]);
            float conf = spc_table_get_float(tbl, i, off[F_CONFIDENCE]);

            int x = static_cast<int>(bx * fw);
            int y = static_cast<int>(by * fh);
            int w = static_cast<int>(bw * fw);
            int h = static_cast<int>(bh * fh);

            cv::rectangle(dst, cv::Rect(x, y, w, h), color, p.thickness);

            if (p.show_info) {
                std::string label;
                if (off_name != SPC_NO_FIELD) {
                    const char* cname = spc_table_get_string(tbl, i, off_name);
                    if (cname && cname[0] != '\0')
                        label = std::format("{} {:.0f}%", cname, conf * 100.0f);
                }
                if (label.empty())
                    label = std::format("#{} {:.0f}%", i, conf * 100.0f);
                if (off[F_AREA] != SPC_NO_FIELD) {
                    float area = spc_table_get_float(tbl, i, off[F_AREA]);
                    label += std::format(" A:{:.1f}", area * 10000.0f);
                }
                if (off[F_MEAN_INTENSITY] != SPC_NO_FIELD) {
                    float intensity = spc_table_get_float(tbl, i, off[F_MEAN_INTENSITY]);
                    label += std::format(" I:{:.0f}", intensity * 255.0f);
                }
                int baseline = 0;
                auto text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                                 0.5, 1, &baseline);
                int text_y = std::max(y - 4, text_size.height + 2);
                cv::putText(dst, label, cv::Point(x, text_y),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv::LINE_AA);
            }
        }
    }

    // optionally letterbox into output resolution (preserves aspect ratio)
    if (p.resize_enable)
    {
        int out_w = p.resize_width;
        int out_h = p.resize_height;
        float scale = std::min(static_cast<float>(out_w) / dst.cols,
                               static_cast<float>(out_h) / dst.rows);
        int scaled_w = static_cast<int>(dst.cols * scale);
        int scaled_h = static_cast<int>(dst.rows * scale);
        int offset_x = (out_w - scaled_w) / 2;
        int offset_y = (out_h - scaled_h) / 2;

        s->resized_mat = cv::Mat::zeros(out_h, out_w, dst.type());
        cv::Mat roi = s->resized_mat(cv::Rect(offset_x, offset_y, scaled_w, scaled_h));
        auto interp = (scaled_h < dst.rows) ? cv::INTER_AREA : cv::INTER_LINEAR;
        cv::resize(dst, roi, roi.size(), 0, 0, interp);

        s->output_frame = *in;
        s->output_frame.width = static_cast<uint32_t>(out_w);
        s->output_frame.height = static_cast<uint32_t>(out_h);
        if (was_grayscale) s->output_frame.format = SPC_PIXEL_FORMAT_RGB24;
        s->output_frame.stride = static_cast<uint32_t>(s->resized_mat.step[0]);
        s->output_frame.data = s->resized_mat.data;
        s->output_frame.gpu_handle = 0;
        s->output_frame.gpu_flags = SPC_GPU_FLAG_NONE;
        s->output_frame.pool_id = 0;
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = &s->output_frame;
    }
    else if (was_grayscale)
    {
        s->output_frame = *in;
        s->output_frame.format = SPC_PIXEL_FORMAT_RGB24;
        s->output_frame.stride = static_cast<uint32_t>(s->converted_rgb.step[0]);
        s->output_frame.data = s->converted_rgb.data;
        s->output_frame.gpu_handle = 0;
        s->output_frame.gpu_flags = SPC_GPU_FLAG_NONE;
        s->output_frame.pool_id = 0;
        outputs[0].type = SPC_DATA_FRAME;
        outputs[0].frame = &s->output_frame;
    }
    else
    {
        set_frame_output(s, in, out_frame, &outputs[0]);
    }

    // The resize and grayscale paths output an image of their own, so the pool
    // frame the input was copied into is not emitted: a frame from
    // host->frame() that is not emitted goes back to the host.
    if ((p.resize_enable || was_grayscale) && out_frame && host->release_frame)
        host->release_frame(host, out_frame);
    return SPC_OK;
}

extern "C" {
SPC_API SpcPlugin spc_plugin = {
    .abi            = SPC_ABI,
    .info           = get_descriptor,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed,
    .param_rules    = param_rules,
};
}
