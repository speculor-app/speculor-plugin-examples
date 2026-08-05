#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

// output schema field indices
enum { F_AMPLITUDE = 0, FIELD_COUNT };

// batch size limits
constexpr uint32_t min_batch = 64;
constexpr uint32_t max_batch = 8192;

// GUI-thread-set parameters, snapshotted on the worker (H6)
struct Params
{
    int32_t wave_type = 0;    // 0=Sine, 1=Square, 2=Triangle, 3=Sawtooth
    float frequency = 440.0f;
    float amplitude = 0.5f;
    int32_t sample_rate = 44100;
};

// internal state
struct WaveGenState
{
    spc::HostServices host;
    SpcTable output_table;
    uint32_t offsets[FIELD_COUNT];
    uint32_t stride;

    // cross-thread parameter block (GUI writes, worker snapshots per frame)
    spc::SharedParams<Params> params;

    // synthesis state
    double phase;         // continuous phase accumulator [0, 1)
    uint64_t sample_count;
    std::chrono::steady_clock::time_point start_time;
};

SPC_PLUGIN_CAST(WaveGenState)
SPC_PLUGIN_HOST_SERVICES(WaveGenState, host)

// generate a single sample from the current phase [0, 1)
static float generate_sample(int32_t wave_type, double phase, float amplitude)
{
    double value = 0.0;
    switch (wave_type) {
        case 0: // sine
            value = std::sin(2.0 * std::numbers::pi * phase);
            break;
        case 1: // square
            value = phase < 0.5 ? 1.0 : -1.0;
            break;
        case 2: // triangle
            value = 4.0 * std::abs(phase - 0.5) - 1.0;
            break;
        case 3: // sawtooth
            value = 2.0 * phase - 1.0;
            break;
    }
    return static_cast<float>(value) * amplitude;
}

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("wave_gen", "Wave Generator", "Signal/Audio/Sources")
        .author("Speculor").version("0.1.0")
        .description("Generates audio waveform samples at real-time rate")
        .maturity(SPC_MATURITY_EXPERIMENTAL)
        .tags({"radio", "common"})
        .output_signal("samples_out", "Samples", {
            {"amplitude", SPC_FIELD_FLOAT},
        })
        .enum_param("wave_type", "Wave Type",
                     {"Sine", "Square", "Triangle", "Sawtooth"}, 0, "Waveform")
            .param_description("Waveform shape: sine, square, sawtooth, or triangle")
        .float_param("frequency", "Frequency (Hz)", 20.0f, 20000.0f, 440.0f, 1.0f, "Waveform")
            .param_description("Signal frequency in Hz")
        .float_param("amplitude", "Amplitude", 0.0f, 1.0f, 0.5f, 0.01f, "Waveform")
            .param_description("Signal amplitude (0 to 1)")
        .int_param("sample_rate", "Sample Rate", 8000, 192000, 44100, 1000, "Output")
            .param_description("Output sample rate in Hz")
        .streaming()
)

// --- lifecycle ---

static SpcPluginInstance* create_instance()
{
    auto* s = new WaveGenState{};
    s->phase = 0.0;
    s->sample_count = 0;

    // precompute field offsets from output schema
    auto* desc = get_descriptor();
    spc_schema_compute_offsets(&desc->ports[0].schema, s->offsets, &s->stride);
    spc_table_init(&s->output_table, s->stride, &desc->ports[0].schema);

    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    spc_table_free(&s->output_table);
    delete s;
}

// --- parameters ---

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value)
{
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_enum (name, value, "wave_type",   p.wave_type)
            || spc::try_set_float(name, value, "frequency",   p.frequency)
            || spc::try_set_float(name, value, "amplitude",   p.amplitude)
            || spc::try_set_int  (name, value, "sample_rate", p.sample_rate);
    });
    return matched ? SPC_OK : SPC_ERR_NOT_FOUND;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_enum (name, out, "wave_type",   p.wave_type)) return SPC_OK;
    if (spc::try_get_float(name, out, "frequency",   p.frequency)) return SPC_OK;
    if (spc::try_get_float(name, out, "amplitude",   p.amplitude)) return SPC_OK;
    if (spc::try_get_int  (name, out, "sample_rate", p.sample_rate)) return SPC_OK;
    return SPC_ERR_NOT_FOUND;
}

// --- streaming ---

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    const Params p = s->params.snapshot();
    s->phase = 0.0;
    s->sample_count = 0;
    s->start_time = std::chrono::steady_clock::now();
    SPC_LOG_INFO(&s->host.cached_log, "Wave Generator started (%.0f Hz %s, %d Hz sample rate)",
                 p.frequency,
                 p.wave_type == 0 ? "sine" : p.wave_type == 1 ? "square"
                     : p.wave_type == 2 ? "triangle" : "sawtooth",
                 p.sample_rate);
    return 0;
}

static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    SPC_LOG_INFO(&s->host.cached_log, "Wave Generator stopped (%llu samples emitted)",
                 static_cast<unsigned long long>(s->sample_count));
    return 0;
}

// --- process ---

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/, uint32_t /*input_count*/,
                   SpcData* outputs, uint32_t output_count)
{
    auto* s = state(inst);
    if (output_count < 1) return -1;

    const Params p = s->params.snapshot();  // one consistent view per frame

    // compute how many samples should have been produced by now
    auto now = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now - s->start_time).count();
    auto expected = static_cast<uint64_t>(
        static_cast<double>(elapsed_us) * static_cast<double>(p.sample_rate) / 1e6);

    auto to_generate = (expected > s->sample_count)
        ? static_cast<uint32_t>(std::min(expected - s->sample_count,
                                         static_cast<uint64_t>(max_batch)))
        : uint32_t{0};

    // sleep until at least min_batch samples are due
    if (to_generate < min_batch) {
        auto needed = s->sample_count + min_batch;
        auto target_us = static_cast<int64_t>(
            static_cast<double>(needed) / static_cast<double>(p.sample_rate) * 1e6);
        auto sleep_us = target_us - elapsed_us;
        if (sleep_us > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));

        // recompute after sleep
        now = std::chrono::steady_clock::now();
        elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now - s->start_time).count();
        expected = static_cast<uint64_t>(
            static_cast<double>(elapsed_us) * static_cast<double>(p.sample_rate) / 1e6);
        to_generate = (expected > s->sample_count)
            ? static_cast<uint32_t>(std::min(expected - s->sample_count,
                                             static_cast<uint64_t>(max_batch)))
            : uint32_t{0};

        if (to_generate == 0) return 0;
    }

    if (spc_table_resize(&s->output_table, to_generate) != 0) return -1;

    double phase_inc = static_cast<double>(p.frequency) / static_cast<double>(p.sample_rate);

    for (uint32_t i = 0; i < to_generate; ++i)
    {
        float sample = generate_sample(p.wave_type, s->phase, p.amplitude);
        spc_table_set_float(&s->output_table, i, s->offsets[F_AMPLITUDE], sample);

        s->phase += phase_inc;
        if (s->phase >= 1.0) s->phase -= 1.0;
    }

    // timestamp from cumulative sample position
    s->output_table.frame_number = s->sample_count / min_batch;
    s->output_table.timestamp_ns = static_cast<int64_t>(
        static_cast<double>(s->sample_count) / static_cast<double>(p.sample_rate) * 1e9);

    s->sample_count += to_generate;

    outputs[0].type = SPC_DATA_SIGNAL;
    outputs[0].table = &s->output_table;
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
