// Starting template: a frame-in / frame-out filter that scales pixel values by
// a `gain` parameter. Copy this directory, rename, and edit process().
//
// This is a STANDALONE CMake project: it has its own project() and finds the
// SDK through CMAKE_PREFIX_PATH, which is the shape you start from. The
// examples/ tree is one parent project with add_subdirectory children, so
// those CMakeLists are a single line and not what you would write.
//
// The plugin is the boundary in speculor/plugin.h, written out: each function
// below fills a slot of SpcPlugin, and SPC_EXPORT_PLUGIN at the bottom defines
// spc_plugin, the one symbol the library exports. A slot the plugin has no use for stays null.

#include <speculor/plugin.h>          // the boundary: every type and call that crosses it
#include <speculor/plugin_helpers.h>  // conveniences compiled into the plugin

#include <algorithm>
#include <new>
#include <vector>

// The parameters in the order the node info declares them. start() and
// params_changed() get the values as one array in this order, so a value's
// position is its identity.
enum Param : uint32_t {
    P_GAIN,
};

// The info slot. SPC_PLUGIN_DESCRIPTOR defines info(), which builds
// the SpcNodeInfo on the first call and keeps it, and all it points at, while
// the library is loaded. DescriptorBuilder is a convenience for filling it.
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_plugin", "My Plugin", "Filters")
        .author("You").version("0.1.0")
        .help("Scales pixel values by a gain factor")
        .maturity(SPC_MATURITY_PREVIEW)
        .float_param("gain", "Gain", 0.0f, 10.0f, 1.0f, 0.1f)   // P_GAIN
            .param_help("Multiplier applied to every pixel")
        .input("in", "Input", SPC_DATA_FRAME)
        .output("out", "Output", SPC_DATA_FRAME)
)

// An instance exists only while its node runs. The engine holds it as an
// opaque SpcPluginInstance* and calls it one call at a time (interrupt
// aside), so it needs no lock.
struct MyPluginState {
    SpcHost* host = nullptr;   // valid until stop() returns
    float gain = 1.0f;
    // the frame used when the host lends none; see process()
    std::vector<uint8_t> buffer;
    SpcFrame current_frame{};
};

static MyPluginState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<MyPluginState*>(instance);
}

// `values` are the host's, valid during the call. Null means the node does
// not run; say why through the host's log first.
static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) MyPluginState{};
    if (!s) return nullptr;
    s->host = host;
    s->gain = values[P_GAIN].f;   // a value is in the member its declared type names: FLOAT is .f
    return reinterpret_cast<SpcPluginInstance*>(s);
}

// Nothing is called on the instance after this.
static void stop(SpcPluginInstance* instance)
{
    delete state(instance);
}

// Every value again, on the node's thread between steps, with bit i of
// `changed` set for each parameter that differs from what this instance last
// had.
static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->gain = values[P_GAIN].f;
}

// The step holds one SpcData per declared input and output, in port order.
// The inputs are the host's, borrowed for the call.
static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const SpcData& in_data = step->inputs[0];
    if (in_data.type != SPC_DATA_FRAME || !in_data.frame) return SPC_INVALID;

    const SpcFrame* in = in_data.frame;
    if (in->format != SPC_PIXEL_FORMAT_RGB24) return SPC_INVALID;

    const uint32_t w = in->width, h = in->height;

    // A frame from the host's pool is written in place and published without
    // a copy. When the host lends none, a frame of the plugin's own is copied.
    SpcFrame* out = host->frame(host, 0, w, h, SPC_PIXEL_FORMAT_RGB24);
    if (!out) s->buffer.resize(static_cast<size_t>(w) * h * 3);
    uint8_t* dst = out ? out->data : s->buffer.data();
    const uint32_t dst_stride = out ? out->stride : w * 3;

    const float gain = s->gain;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src_row = in->data + static_cast<size_t>(y) * in->stride;
        uint8_t* dst_row       = dst      + static_cast<size_t>(y) * dst_stride;
        for (uint32_t x = 0; x < w * 3; ++x) {
            const float v = static_cast<float>(src_row[x]) * gain;
            dst_row[x] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
        }
    }

    SpcData& out_data = step->outputs[0];
    out_data.type = SPC_DATA_FRAME;
    if (out) {
        out->timestamp_ns = in->timestamp_ns;
        out->sequence     = in->sequence;   // what it was made from, so data can be matched
        out_data.frame    = out;            // the engine's from here on, whatever process() returns
    } else {
        s->current_frame.data         = s->buffer.data();
        s->current_frame.width        = w;
        s->current_frame.height       = h;
        s->current_frame.stride       = w * 3;
        s->current_frame.format       = SPC_PIXEL_FORMAT_RGB24;
        s->current_frame.timestamp_ns = in->timestamp_ns;
        s->current_frame.sequence     = in->sequence;
        out_data.frame = &s->current_frame;
    }
    return SPC_OK;
}

// spc_plugin, the one symbol the engine looks up, with its `abi` set to
// SPC_ABI: the engine loads the plugin only when that equals its own. A slot
// left out is null, a call the plugin does without. The slots, in the order
// SpcPlugin declares them (which designated initializers must follow): info,
// start, stop, process, interrupt, params_changed, record_gpu, ui_input,
// control_reply, param_rules, list_options.
SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
