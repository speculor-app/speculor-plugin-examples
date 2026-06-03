#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>
#include <cv_helpers.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <format>
#include <vector>

// input schema field indices
enum { F_X = 0, F_Y, F_W, F_H, F_CONFIDENCE, IN_FIELD_COUNT };

struct BboxDisplayState {
    spc::HostServices host;

    uint32_t color_rgba = spc::color_pack(0, 255, 0);
    int32_t thickness = 2;
    int32_t show_info = 1;
    int32_t resize_enable = 0;
    int32_t resize_width = 1920;
    int32_t resize_height = 1080;

    std::vector<uint8_t> buffer;
    SpcFrame output_frame{};
    cv::Mat converted_rgb; // keeps RGB conversion alive across output
    cv::Mat resized_mat;   // keeps resized frame alive across output

    // cached input field offsets (resolved from incoming table schema)
    uint32_t in_off_x{};
    uint32_t in_off_y{};
    uint32_t in_off_w{};
    uint32_t in_off_h{};
    uint32_t in_off_confidence{};
    uint32_t in_off_area{};
    uint32_t in_off_intensity{};
    uint32_t in_off_class_name{};
    bool in_offsets_resolved = false;
    bool has_area = false;
    bool has_intensity = false;
    bool has_class_name = false;

};

SPC_PLUGIN(BboxDisplayState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("bbox_display", "BB Display", "Renderers/Overlays")
        .author("Speculor").version("0.1.0")
        .description("Draws bounding boxes on an image frame")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"tracking", "image"})
        .input_table("boxes_in", "Boxes", {
            {"x", SPC_FIELD_FLOAT}, {"y", SPC_FIELD_FLOAT},
            {"w", SPC_FIELD_FLOAT}, {"h", SPC_FIELD_FLOAT},
            {"confidence", SPC_FIELD_FLOAT},
        }, 4, SPC_CONSUME_LATEST)
        .input("image_in", "Image", SPC_DATA_FRAME)
        .output("image_out", "Image Out", SPC_DATA_FRAME)
        .color_param("color", "Color", spc::color_pack(0, 255, 0))
            .param_description("Bounding box outline color")
        .int_param("thickness", "Thickness", 1, 10, 2, 1)
            .param_description("Bounding box line thickness in pixels")
        .bool_param("show_info", "Show Info", true)
            .param_description("Show confidence and class label next to each box")
        .bool_param("resize_enable", "Resize", false, "Output Resolution")
            .param_description("Resize the output image to a specific resolution")
        .int_param("resize_width", "Width", 32, 7680, 1920, 1, "Output Resolution")
            .param_description("Output image width in pixels")
        .int_param("resize_height", "Height", 32, 4320, 1080, 1, "Output Resolution")
            .param_description("Output image height in pixels")
        .frame_alloc()
)

// --- parameters ---------------------------------------------------------------

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value)
{
    auto* s = state(inst);
    if (spc::try_set_color(name, value, "color", s->color_rgba)) return 0;
    if (spc::try_set_int(name, value, "thickness", s->thickness)) return 0;
    if (spc::try_set_bool(name, value, "show_info", s->show_info)) return 0;
    if (spc::try_set_bool(name, value, "resize_enable", s->resize_enable)) return 0;
    if (spc::try_set_int(name, value, "resize_width", s->resize_width)) return 0;
    if (spc::try_set_int(name, value, "resize_height", s->resize_height)) return 0;
    return -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out)
{
    auto* s = state(inst);
    if (spc::try_get_color(name, out, "color", s->color_rgba)) return 0;
    if (spc::try_get_int(name, out, "thickness", s->thickness)) return 0;
    if (spc::try_get_bool(name, out, "show_info", s->show_info)) return 0;
    if (spc::try_get_bool(name, out, "resize_enable", s->resize_enable)) return 0;
    if (spc::try_get_int(name, out, "resize_width", s->resize_width)) {
        if (!s->resize_enable) out->flags |= SPC_PARAM_FLAG_DISABLED;
        return 0;
    }
    if (spc::try_get_int(name, out, "resize_height", s->resize_height)) {
        if (!s->resize_enable) out->flags |= SPC_PARAM_FLAG_DISABLED;
        return 0;
    }
    return -1;
}

// --- helpers ------------------------------------------------------------------

// acquire pool frame or fallback buffer, copy input, return cv::Mat wrapping dst
static cv::Mat acquire_and_copy_frame(BboxDisplayState* s, const SpcFrame* in,
                                      int cv_type, SpcFrame** out_pool_frame)
{
    *out_pool_frame = s->host.acquire_frame(0, in->width, in->height, in->format);

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

// resolve field offsets from incoming table schema on first use
static void resolve_bbox_offsets(BboxDisplayState* s, const SpcTable* tbl)
{
    if (s->in_offsets_resolved || !tbl->schema) return;

    s->in_off_x = spc_schema_field_offset(tbl->schema, "x");
    s->in_off_y = spc_schema_field_offset(tbl->schema, "y");
    s->in_off_w = spc_schema_field_offset(tbl->schema, "w");
    s->in_off_h = spc_schema_field_offset(tbl->schema, "h");
    s->in_off_confidence = spc_schema_field_offset(tbl->schema, "confidence");
    s->in_off_area = spc_schema_field_offset(tbl->schema, "area");
    s->has_area = (s->in_off_area != UINT32_MAX);
    s->in_off_intensity = spc_schema_field_offset(tbl->schema, "mean_intensity");
    s->has_intensity = (s->in_off_intensity != UINT32_MAX);
    // prefer label_name (smoothed by track_class_enricher) over class_name
    // (raw detector / mode-of-history vote) — same convention as track_display
    s->in_off_class_name = spc_schema_field_offset(tbl->schema, "label_name");
    s->has_class_name = (s->in_off_class_name != UINT32_MAX);
    if (!s->has_class_name) {
        s->in_off_class_name = spc_schema_field_offset(tbl->schema, "class_name");
        s->has_class_name = (s->in_off_class_name != UINT32_MAX);
    }
    s->in_offsets_resolved = true;
}

// set output data (pool path or fallback path)
static void set_frame_output(BboxDisplayState* s, const SpcFrame* in,
                             SpcFrame* pool_frame, SpcData* output)
{
    if (pool_frame) {
        pool_frame->timestamp_us = in->timestamp_us;
        pool_frame->frame_number = in->frame_number;
        output->type = SPC_DATA_FRAME;
        output->frame = pool_frame;
    } else {
        s->output_frame = *in;
        s->output_frame.data = s->buffer.data();
        output->type = SPC_DATA_FRAME;
        output->frame = &s->output_frame;
    }
}

// --- process ------------------------------------------------------------------

static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count)
{
    auto* s = state(inst);
    if (input_count < 2 || output_count < 1) return -1;
    if (inputs[1].type != SPC_DATA_FRAME || !inputs[1].frame) return -1;

    const SpcFrame* in = inputs[1].frame;
    int cv_type = spc::cv_type_for_format(in->format);
    if (cv_type < 0) return -1;

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
    if (inputs[0].type == SPC_DATA_TABLE && inputs[0].table) {
        const SpcTable* tbl = inputs[0].table;
        resolve_bbox_offsets(s, tbl);

        uint8_t cr, cg, cb, ca;
        spc::color_unpack(s->color_rgba, cr, cg, cb, ca);
        const cv::Scalar color(cr, cg, cb); // RGB
        auto fw = static_cast<float>(in->width);
        auto fh = static_cast<float>(in->height);

        for (uint32_t i = 0; i < tbl->record_count; ++i) {
            float bx = spc_table_get_float(tbl, i, s->in_off_x);
            float by = spc_table_get_float(tbl, i, s->in_off_y);
            float bw = spc_table_get_float(tbl, i, s->in_off_w);
            float bh = spc_table_get_float(tbl, i, s->in_off_h);
            float conf = spc_table_get_float(tbl, i, s->in_off_confidence);

            int x = static_cast<int>(bx * fw);
            int y = static_cast<int>(by * fh);
            int w = static_cast<int>(bw * fw);
            int h = static_cast<int>(bh * fh);

            cv::rectangle(dst, cv::Rect(x, y, w, h), color, s->thickness);

            if (s->show_info) {
                std::string label;
                if (s->has_class_name) {
                    const char* cname = spc_table_get_string(tbl, i, s->in_off_class_name);
                    if (cname && cname[0] != '\0')
                        label = std::format("{} {:.0f}%", cname, conf * 100.0f);
                }
                if (label.empty())
                    label = std::format("#{} {:.0f}%", i, conf * 100.0f);
                if (s->has_area) {
                    float area = spc_table_get_float(tbl, i, s->in_off_area);
                    label += std::format(" A:{:.1f}", area * 10000.0f);
                }
                if (s->has_intensity) {
                    float intensity = spc_table_get_float(tbl, i, s->in_off_intensity);
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
    if (s->resize_enable)
    {
        int out_w = s->resize_width;
        int out_h = s->resize_height;
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
    return 0;
}

// --- export -------------------------------------------------------------------

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .set_host_services = set_host_services
)
