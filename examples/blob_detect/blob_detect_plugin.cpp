// Connected-component blob detection on a foreground mask, with spclib: one
// TABLE record per blob, and the source image passed through untouched.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>
#include <speculor/spclib_log_bridge.h>

#include <blobs/connectedBlobDetection.hpp>

#include <opencv2/core.hpp>

#include <algorithm>
#include <memory>
#include <new>
#include <vector>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_CONNECTIVITY,
    P_ENABLE_JOIN,
    P_SIZE_THRESHOLD,
    P_AREA_THRESHOLD,
    P_MIN_DISTANCE,
    P_MAX_BLOBS,
    P_BBOX_PADDING,
};

// the ports, in declaration order
enum Input : uint32_t { IN_MASK, IN_IMAGE };
enum Output : uint32_t { OUT_BOXES, OUT_IMAGE };

// output schema field indices
enum { F_X = 0, F_Y, F_W, F_H, F_CONFIDENCE, F_AREA, F_MEAN_INTENSITY,
       F_CENTROID_X, F_CENTROID_Y, F_SOLIDITY, F_ASPECT_RATIO, FIELD_COUNT };

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("blob_detect", "Blob Detect", "Analysis/Detection")
        .author("Speculor").version("0.2.0")
        .help("Connected-component blob detection with optional merging and shape filtering")
        .maturity(SPC_MATURITY_STABLE)
        .tags({"image", "tracking"})
        .input("mask_in", "FG Mask", SPC_DATA_FRAME, 32, SPC_CONSUME_FIFO)
        // SPC_INPUT_GPU_OK: a GPU-resident image arrives as it is, not
        // downloaded; it is passed on, and read only when it has CPU pixels
        .input("image_in", "Image", SPC_DATA_FRAME, 32, SPC_CONSUME_FIFO).gpu_ok()
        .output_table("boxes_out", "Boxes", {
            {"x", SPC_FIELD_FLOAT}, {"y", SPC_FIELD_FLOAT},
            {"w", SPC_FIELD_FLOAT}, {"h", SPC_FIELD_FLOAT},
            {"confidence", SPC_FIELD_FLOAT},
            {"area", SPC_FIELD_FLOAT},
            {"mean_intensity", SPC_FIELD_FLOAT},
            {"centroid_x", SPC_FIELD_FLOAT}, {"centroid_y", SPC_FIELD_FLOAT},
            {"solidity", SPC_FIELD_FLOAT},
            {"aspect_ratio", SPC_FIELD_FLOAT},
        })
        .output("image_out", "Image", SPC_DATA_FRAME)
        .enum_param("connectivity", "Connectivity", {"4-connected", "8-connected"}, 1, "Detection")
            .param_help("Pixel neighborhood for connected component search")
        .bool_param("enable_join", "Join Nearby", true, "Detection")
            .param_help("Merge nearby blobs within Min Distance into single detections")
        .int_param("size_threshold", "Size Threshold", 2, 100, 5, 1, "Detection")
            .param_help("Minimum bounding box size in pixels to accept a detection")
        .int_param("area_threshold", "Area Threshold", 4, 10000, 25, 1, "Detection")
            .param_help("Minimum pixel area of a connected component to count as a blob")
        .int_param("min_distance", "Min Distance", 2, 200, 25, 1, "Detection")
            .param_help("Minimum distance in pixels between blob centers to merge nearby detections")
        .int_param("max_blobs", "Max Blobs", 1, 500, 100, 1, "Detection")
            .param_help("Maximum number of blobs to report per frame")
        .float_param("bbox_padding", "Bbox Padding", 0.0f, 0.5f, 0.0f, 0.01f, "Detection")
            .param_help("Symmetric margin added around each output bbox, as a fraction of bbox size. Clamped to frame bounds. Does not affect centroid, area, mean intensity, solidity, or aspect ratio.")
)

struct Params
{
    int32_t connectivity;   // 0=Four, 1=Eight
    int32_t enable_join;
    int32_t size_threshold;
    int32_t area_threshold;
    int32_t min_distance;
    int32_t max_blobs;
    float bbox_padding;     // symmetric expansion of output bbox, fraction of bbox size
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.connectivity   = values[P_CONNECTIVITY].choice.index;
    p.enable_join    = values[P_ENABLE_JOIN].i;
    p.size_threshold = values[P_SIZE_THRESHOLD].i;
    p.area_threshold = values[P_AREA_THRESHOLD].i;
    p.min_distance   = values[P_MIN_DISTANCE].i;
    p.max_blobs      = values[P_MAX_BLOBS].i;
    p.bbox_padding   = values[P_BBOX_PADDING].f;
    return p;
}

struct BlobDetectState
{
    SpcHost* host = nullptr;   // valid until stop() returns
    SpcLogContext log{};
    Params params{};
    std::unique_ptr<spclib::blobs::ConnectedBlobDetection> detector;
    cv::Mat input_image;
    std::vector<spclib::blobs::BlobInfo> blobs;
};

static BlobDetectState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<BlobDetectState*>(instance);
}

static spclib::blobs::ConnectedBlobDetectionParams build_params(const Params& sp)
{
    spclib::blobs::ConnectedBlobDetectionParams p;
    p.set_size_threshold(sp.size_threshold);
    p.set_area_threshold(sp.area_threshold);
    p.set_min_distance(sp.min_distance);
    p.set_max_blobs(sp.max_blobs);
    p.enable_join = sp.enable_join != 0;
    p.connectivity = sp.connectivity == 0
        ? spclib::blobs::Connectivity::Four
        : spclib::blobs::Connectivity::Eight;
    return p;
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) BlobDetectState{};
    if (!s) return nullptr;
    s->host = host;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    s->params = read_params(values);
    s->detector = std::make_unique<spclib::blobs::ConnectedBlobDetection>(
        build_params(s->params));
    SPC_LOG_INFO(&s->log, "Blob Detect started");
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    SPC_LOG_INFO(&s->log, "Blob Detect stopped");
    delete s;
}

// On the node's thread, between steps: the detector can be changed in place.
static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    auto* s = state(instance);
    s->params = read_params(values);
    const Params& p = s->params;
    s->detector->set_size_threshold(p.size_threshold);
    s->detector->set_area_threshold(p.area_threshold);
    s->detector->set_min_distance(p.min_distance);
    s->detector->set_max_blobs(p.max_blobs);
    s->detector->set_enable_join(p.enable_join != 0);
    s->detector->set_connectivity(p.connectivity == 0
        ? spclib::blobs::Connectivity::Four
        : spclib::blobs::Connectivity::Eight);
}

// Which parameters the values leave disabled: a function of the values alone,
// called with no instance, on any thread. `states` has one entry per parameter.
static void param_rules(const SpcValue* values, uint32_t* states)
{
    if (values[P_ENABLE_JOIN].i == 0) states[P_MIN_DISTANCE] |= SPC_PARAM_STATE_DISABLED;
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const Params& p = s->params;
    const SpcData* inputs = step->inputs;

    // spclib logs through a sink kept per thread: route this thread's to the
    // node's log before calling into it
    spc::install_spclib_log_bridge(&s->log);

    if (inputs[IN_MASK].type != SPC_DATA_FRAME || !inputs[IN_MASK].frame) return SPC_SKIP;

    SpcFrame* in_frame = inputs[IN_MASK].frame;
    if (in_frame->format != SPC_PIXEL_FORMAT_GRAY8) return SPC_FAILED;

    // a GPU-resident mask needs its pixels on the CPU first
    if ((in_frame->gpu_flags & SPC_GPU_FLAG_RESIDENT) &&
        !(in_frame->gpu_flags & SPC_GPU_FLAG_CPU_VALID)) {
        host->ensure_cpu_data(host, in_frame);
    }

    // wrap input mask directly (zero-copy)
    s->input_image = cv::Mat(static_cast<int>(in_frame->height),
                            static_cast<int>(in_frame->width),
                            CV_8UC1, in_frame->data,
                            static_cast<size_t>(in_frame->stride));

    s->blobs.clear();
    s->detector->detect(s->input_image, s->blobs);

    auto count = static_cast<uint32_t>(s->blobs.size());

    // one record per blob in a table the host lends, valid until process()
    // returns; spc_table_set_* (table_helpers.h) write a field at its offset
    SpcTable* out = host->table(host, OUT_BOXES, count);
    if (!out) return SPC_FAILED;
    const uint32_t* off = out->field_offsets;

    // optional: compute mean intensity from image_in if available
    SpcFrame* img_frame = nullptr;
    if (inputs[IN_IMAGE].type == SPC_DATA_FRAME && inputs[IN_IMAGE].frame)
    {
        img_frame = inputs[IN_IMAGE].frame;
    }

    // convert pixel-space blobs to normalized table records
    auto fw = static_cast<float>(in_frame->width);
    auto fh = static_cast<float>(in_frame->height);
    const float total_pixels = fw * fh;

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& b = s->blobs[i];
        const auto& r = b.bbox;

        // Symmetric padding in pixel space, clamped to frame bounds.
        // mean_intensity below still uses the original `r` so it reflects
        // the moving region, not the padded margin.
        const float pad_x = static_cast<float>(r.width)  * p.bbox_padding;
        const float pad_y = static_cast<float>(r.height) * p.bbox_padding;
        const float px0 = std::max(0.0f, static_cast<float>(r.x) - pad_x);
        const float py0 = std::max(0.0f, static_cast<float>(r.y) - pad_y);
        const float px1 = std::min(fw,   static_cast<float>(r.x + r.width)  + pad_x);
        const float py1 = std::min(fh,   static_cast<float>(r.y + r.height) + pad_y);

        spc_table_set_float(out, i, off[F_X], px0 / fw);
        spc_table_set_float(out, i, off[F_Y], py0 / fh);
        spc_table_set_float(out, i, off[F_W], (px1 - px0) / fw);
        spc_table_set_float(out, i, off[F_H], (py1 - py0) / fh);
        spc_table_set_float(out, i, off[F_CONFIDENCE], 1.0f);
        spc_table_set_float(out, i, off[F_AREA],
                           static_cast<float>(b.pixel_count) / total_pixels);

        // compute mean intensity from original image at bbox region
        float mean_intensity = 0.f;
        if (img_frame && img_frame->data)
        {
            const int x0 = std::max(0, r.x);
            const int y0 = std::max(0, r.y);
            const int x1 = std::min(static_cast<int>(img_frame->width), r.x + r.width);
            const int y1 = std::min(static_cast<int>(img_frame->height), r.y + r.height);
            const int channels = (img_frame->format == SPC_PIXEL_FORMAT_GRAY8) ? 1 :
                                 (img_frame->format == SPC_PIXEL_FORMAT_BGR24 ||
                                  img_frame->format == SPC_PIXEL_FORMAT_RGB24) ? 3 : 1;
            long sum = 0;
            int sample_count = 0;
            for (int py = y0; py < y1; ++py)
            {
                const uint8_t* row = img_frame->data + py * img_frame->stride;
                for (int px = x0; px < x1; ++px)
                {
                    if (channels == 1)
                    {
                        sum += row[px];
                    }
                    else
                    {
                        const int base = px * channels;
                        sum += (row[base] + row[base + 1] + row[base + 2]) / 3;
                    }
                    ++sample_count;
                }
            }
            if (sample_count > 0)
            {
                mean_intensity = static_cast<float>(sum) / (static_cast<float>(sample_count) * 255.f);
            }
        }
        spc_table_set_float(out, i, off[F_MEAN_INTENSITY], mean_intensity);

        spc_table_set_float(out, i, off[F_CENTROID_X], b.centroid_x / fw);
        spc_table_set_float(out, i, off[F_CENTROID_Y], b.centroid_y / fh);

        spc_table_set_float(out, i, off[F_SOLIDITY],
                           spclib::blobs::solidity(b));
        spc_table_set_float(out, i, off[F_ASPECT_RATIO],
                           spclib::blobs::aspect_ratio(b));
    }

    out->sequence = in_frame->sequence;
    out->timestamp_ns = in_frame->timestamp_ns;

    step->outputs[OUT_BOXES].type = SPC_DATA_TABLE;
    step->outputs[OUT_BOXES].table = out;
    if (img_frame) {
        // an input frame passed on unchanged: the engine shares it, no copy
        step->outputs[OUT_IMAGE].type = SPC_DATA_FRAME;
        step->outputs[OUT_IMAGE].frame = img_frame;
    }
    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed,
    .param_rules    = param_rules
)
