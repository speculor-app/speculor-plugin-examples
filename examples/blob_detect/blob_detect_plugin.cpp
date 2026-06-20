#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>
#include <speculor/spclib_log_bridge.h>

#include <blobs/connectedBlobDetection.hpp>

#include <opencv2/core.hpp>

#include <atomic>
#include <memory>
#include <vector>

// output schema field indices
enum { F_X = 0, F_Y, F_W, F_H, F_CONFIDENCE, F_AREA, F_MEAN_INTENSITY,
       F_CENTROID_X, F_CENTROID_Y, F_SOLIDITY, F_ASPECT_RATIO, FIELD_COUNT };

// GUI-thread-set parameters, snapshotted on the worker (H6). The detector is
// worker-owned (its setters write fields detect() reads), so set_parameter
// only mutates this block and the worker applies the snapshot on a dirty flag.
struct Params
{
    int32_t size_threshold = 5;
    int32_t area_threshold = 25;
    int32_t min_distance = 25;
    int32_t max_blobs = 100;
    int32_t enable_join = 1;
    int32_t connectivity = 1; // 0=Four, 1=Eight
    float bbox_padding = 0.0f; // symmetric expansion of output bbox, fraction of bbox size
};

// internal state
struct BlobDetectState
{
    spc::HostServices host;
    std::unique_ptr<spclib::blobs::ConnectedBlobDetection> detector;
    cv::Mat input_image;
    std::vector<spclib::blobs::BlobInfo> blobs;
    SpcTable output_table;
    uint32_t offsets[FIELD_COUNT];
    uint32_t stride;

    // cross-thread parameter block (GUI writes, worker snapshots per frame)
    spc::SharedParams<Params> params;
    std::atomic<bool> params_dirty{false};
};

SPC_PLUGIN_CAST(BlobDetectState)
SPC_PLUGIN_HOST_SERVICES(BlobDetectState, host)


SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("blob_detect", "Blob Detect", "Analysis/Detection")
        .author("Speculor").version("0.2.0")
        .description("Connected-component blob detection with optional merging and shape filtering")
        .maturity(SPC_MATURITY_STABLE)
        .tags({"image", "tracking"})
        .input("mask_in", "FG Mask", SPC_DATA_FRAME, 32, SPC_CONSUME_FIFO)
        .input("image_in", "Image", SPC_DATA_FRAME, 32, SPC_CONSUME_FIFO).passthrough()
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
            .param_description("Pixel neighborhood for connected component search")
        .bool_param("enable_join", "Join Nearby", true, "Detection")
            .param_description("Merge nearby blobs within Min Distance into single detections")
        .int_param("size_threshold", "Size Threshold", 2, 100, 5, 1, "Detection")
            .param_description("Minimum bounding box size in pixels to accept a detection")
        .int_param("area_threshold", "Area Threshold", 4, 10000, 25, 1, "Detection")
            .param_description("Minimum pixel area of a connected component to count as a blob")
        .int_param("min_distance", "Min Distance", 2, 200, 25, 1, "Detection")
            .param_description("Minimum distance in pixels between blob centers to merge nearby detections")
        .int_param("max_blobs", "Max Blobs", 1, 500, 100, 1, "Detection")
            .param_description("Maximum number of blobs to report per frame")
        .float_param("bbox_padding", "Bbox Padding", 0.0f, 0.5f, 0.0f, 0.01f, "Detection")
            .param_description("Symmetric margin added around each output bbox, as a fraction of bbox size. Clamped to frame bounds. Does not affect centroid, area, mean intensity, solidity, or aspect ratio.")
        .streaming().frame_alloc()
)

// --- lifecycle ---

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

// apply a parameter snapshot to the live detector. Worker thread only.
static void apply_params(BlobDetectState* s, const Params& sp)
{
    s->detector->set_size_threshold(sp.size_threshold);
    s->detector->set_area_threshold(sp.area_threshold);
    s->detector->set_min_distance(sp.min_distance);
    s->detector->set_max_blobs(sp.max_blobs);
    s->detector->set_enable_join(sp.enable_join != 0);
    s->detector->set_connectivity(sp.connectivity == 0
        ? spclib::blobs::Connectivity::Four
        : spclib::blobs::Connectivity::Eight);
}

static SpcPluginInstance* create_instance()
{
    auto* s = new BlobDetectState{};

    auto* desc = get_descriptor();
    spc_schema_compute_offsets(&desc->ports[2].schema, s->offsets, &s->stride);
    spc_table_init(&s->output_table, s->stride, &desc->ports[2].schema);

    s->detector = std::make_unique<spclib::blobs::ConnectedBlobDetection>(
        build_params(s->params.snapshot()));

    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    s->detector.reset();
    spc_table_free(&s->output_table);
    delete s;
}

// --- parameters ---

static int set_parameter(SpcPluginInstance* inst, const char* name, const SpcParameterDesc* value)
{
    auto* s = state(inst);
    // Mutate the shared block only; the worker applies it to the detector on
    // the dirty flag (detector setters must not run concurrently with detect()).
    bool matched = s->params.update([&](Params& p) {
        return spc::try_set_int  (name, value, "size_threshold", p.size_threshold)
            || spc::try_set_int  (name, value, "area_threshold", p.area_threshold)
            || spc::try_set_int  (name, value, "min_distance", p.min_distance)
            || spc::try_set_int  (name, value, "max_blobs", p.max_blobs)
            || spc::try_set_bool (name, value, "enable_join", p.enable_join)
            || spc::try_set_enum (name, value, "connectivity", p.connectivity)
            || spc::try_set_float(name, value, "bbox_padding", p.bbox_padding);
    });
    if (matched) s->params_dirty.store(true, std::memory_order_release);
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name, SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_int(name, out, "size_threshold", p.size_threshold)) return 0;
    if (spc::try_get_int(name, out, "area_threshold", p.area_threshold)) return 0;
    if (spc::try_get_int(name, out, "min_distance", p.min_distance)) {
        if (!p.enable_join) out->flags |= SPC_PARAM_FLAG_DISABLED;
        return 0;
    }
    if (spc::try_get_int(name, out, "max_blobs", p.max_blobs)) return 0;
    if (spc::try_get_bool(name, out, "enable_join", p.enable_join)) return 0;
    if (spc::try_get_enum(name, out, "connectivity", p.connectivity)) return 0;
    if (spc::try_get_float(name, out, "bbox_padding", p.bbox_padding)) return 0;
    return -1;
}

// --- streaming ---

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    spc::install_spclib_log_bridge(&s->host.cached_log);
    // recreate detector with current params on start
    s->detector = std::make_unique<spclib::blobs::ConnectedBlobDetection>(
        build_params(s->params.snapshot()));
    s->params_dirty.store(false, std::memory_order_release);
    SPC_LOG_INFO(&s->host.cached_log, "Blob Detect started");
    return 0;
}

static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    SPC_LOG_INFO(&s->host.cached_log, "Blob Detect stopped");
    return 0;
}

// --- process ---

static int process(SpcPluginInstance* inst, const SpcData* inputs, uint32_t input_count,
                   SpcData* outputs, uint32_t output_count)
{
    auto* s = state(inst);

    // ensure spclib log bridge is installed on this (node) thread
    thread_local bool bridge_installed = false;
    if (!bridge_installed) {
        spc::install_spclib_log_bridge(&s->host.cached_log);
        bridge_installed = true;
    }

    if (input_count < 1 || output_count < 2) return -1;
    if (inputs[0].type != SPC_DATA_FRAME || !inputs[0].frame) return -1;

    SpcFrame* in_frame = const_cast<SpcFrame*>(inputs[0].frame);
    if (in_frame->format != SPC_PIXEL_FORMAT_GRAY8) return -1;

    // apply any GUI-thread parameter change to the worker-owned detector
    const Params p = s->params.snapshot();
    if (s->params_dirty.exchange(false, std::memory_order_acquire))
        apply_params(s, p);

    // ensure CPU data is available (GPU-resident frames need lazy download)
    if ((in_frame->gpu_flags & SPC_GPU_FLAG_RESIDENT) &&
        !(in_frame->gpu_flags & SPC_GPU_FLAG_CPU_VALID)) {
        s->host.ensure_cpu_data(in_frame);
    }

    // wrap input mask directly (zero-copy)
    s->input_image = cv::Mat(static_cast<int>(in_frame->height),
                            static_cast<int>(in_frame->width),
                            CV_8UC1, in_frame->data,
                            static_cast<size_t>(in_frame->stride));

    s->blobs.clear();
    s->detector->detect(s->input_image, s->blobs);

    auto count = static_cast<uint32_t>(s->blobs.size());

    if (spc_table_resize(&s->output_table, count) != 0) return -1;

    // optional: compute mean intensity from image_in if available
    const SpcFrame* img_frame = nullptr;
    if (input_count > 1 && inputs[1].type == SPC_DATA_FRAME && inputs[1].frame)
    {
        img_frame = inputs[1].frame;
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

        spc_table_set_float(&s->output_table, i, s->offsets[F_X], px0 / fw);
        spc_table_set_float(&s->output_table, i, s->offsets[F_Y], py0 / fh);
        spc_table_set_float(&s->output_table, i, s->offsets[F_W], (px1 - px0) / fw);
        spc_table_set_float(&s->output_table, i, s->offsets[F_H], (py1 - py0) / fh);
        spc_table_set_float(&s->output_table, i, s->offsets[F_CONFIDENCE], 1.0f);
        spc_table_set_float(&s->output_table, i, s->offsets[F_AREA],
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
        spc_table_set_float(&s->output_table, i, s->offsets[F_MEAN_INTENSITY], mean_intensity);

        spc_table_set_float(&s->output_table, i, s->offsets[F_CENTROID_X], b.centroid_x / fw);
        spc_table_set_float(&s->output_table, i, s->offsets[F_CENTROID_Y], b.centroid_y / fh);

        spc_table_set_float(&s->output_table, i, s->offsets[F_SOLIDITY],
                           spclib::blobs::solidity(b));
        spc_table_set_float(&s->output_table, i, s->offsets[F_ASPECT_RATIO],
                           spclib::blobs::aspect_ratio(b));
    }

    s->output_table.frame_number = in_frame->frame_number;
    s->output_table.timestamp_ns = in_frame->timestamp_ns;

    outputs[0].type = SPC_DATA_TABLE;
    outputs[0].table = &s->output_table;
    if (img_frame) {
        outputs[1].type = SPC_DATA_FRAME;
        outputs[1].frame = const_cast<SpcFrame*>(img_frame);
    }
    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .start             = start,
    .stop              = stop,
    .set_host_services = set_host_services
)
