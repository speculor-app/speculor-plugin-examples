// Generates a waveform at real-time rate on a SIGNAL output: blocks of samples
// written into tables the host lends.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>
#include <numbers>
#include <thread>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_WAVE_TYPE,
    P_FREQUENCY,
    P_AMPLITUDE,
    P_SAMPLE_RATE,
};

// output schema field indices
enum { F_AMPLITUDE = 0, FIELD_COUNT };

// batch size limits
constexpr uint32_t min_batch = 64;
constexpr uint32_t max_batch = 8192;

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("wave_gen", "Wave Generator", "Signal/Audio/Sources")
        .author("Speculor").version("0.1.0")
        .help("Generates audio waveform samples at real-time rate")
        .maturity(SPC_MATURITY_EXPERIMENTAL)
        .tags({"radio", "common"})
        .output_signal("samples_out", "Samples", {
            {"amplitude", SPC_FIELD_FLOAT},
        })
        .enum_param("wave_type", "Wave Type",
                     {"Sine", "Square", "Triangle", "Sawtooth"}, 0, "Waveform")
            .param_help("Waveform shape: sine, square, sawtooth, or triangle")
        .float_param("frequency", "Frequency (Hz)", 20.0f, 20000.0f, 440.0f, 1.0f, "Waveform")
            .param_help("Signal frequency in Hz")
        .float_param("amplitude", "Amplitude", 0.0f, 1.0f, 0.5f, 0.01f, "Waveform")
            .param_help("Signal amplitude (0 to 1)")
        .int_param("sample_rate", "Sample Rate", 8000, 192000, 44100, 1000, "Output")
            .param_help("Output sample rate in Hz")
)

struct Params
{
    int32_t wave_type;    // 0=Sine, 1=Square, 2=Triangle, 3=Sawtooth
    float frequency;
    float amplitude;
    int32_t sample_rate;
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.wave_type   = values[P_WAVE_TYPE].choice.index;   // ENUM: the position of the option
    p.frequency   = values[P_FREQUENCY].f;
    p.amplitude   = values[P_AMPLITUDE].f;
    p.sample_rate = values[P_SAMPLE_RATE].i;
    return p;
}

struct WaveGenState
{
    SpcHost* host = nullptr;     // valid until stop() returns
    SpcLogContext log{};
    Params params{};

    // synthesis state
    double phase = 0.0;          // continuous phase accumulator [0, 1)
    uint64_t sample_count = 0;
    std::chrono::steady_clock::time_point start_time;
};

static WaveGenState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<WaveGenState*>(instance);
}

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

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) WaveGenState{};
    if (!s) return nullptr;
    s->host = host;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    s->params = read_params(values);
    s->start_time = std::chrono::steady_clock::now();
    const Params& p = s->params;
    SPC_LOG_INFO(&s->log, "Wave Generator started (%.0f Hz %s, %d Hz sample rate)",
                 p.frequency,
                 p.wave_type == 0 ? "sine" : p.wave_type == 1 ? "square"
                     : p.wave_type == 2 ? "triangle" : "sawtooth",
                 p.sample_rate);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    SPC_LOG_INFO(&s->log, "Wave Generator stopped (%llu samples emitted)",
                 static_cast<unsigned long long>(s->sample_count));
    delete s;
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->params = read_params(values);
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const Params& p = s->params;

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

        if (to_generate == 0) return SPC_OK;
    }

    // The host lends the output table: to_generate zeroed records of the
    // port's schema, the host's until process() returns.
    SpcTable* out = host->table(host, 0, to_generate);
    if (!out) return SPC_FAILED;
    // where each field of the port is in a record, in declaration order
    const uint32_t* off = out->field_offsets;

    double phase_inc = static_cast<double>(p.frequency) / static_cast<double>(p.sample_rate);

    for (uint32_t i = 0; i < to_generate; ++i)
    {
        float sample = generate_sample(p.wave_type, s->phase, p.amplitude);
        // spc_table_set_float (table_helpers.h) writes the float at that offset of record i
        spc_table_set_float(out, i, off[F_AMPLITUDE], sample);

        s->phase += phase_inc;
        if (s->phase >= 1.0) s->phase -= 1.0;
    }

    out->sequence = s->sample_count / min_batch;
    // the engine's UTC clock, which every timestamp is on
    out->timestamp_ns = host->now_utc_ns(host);

    s->sample_count += to_generate;

    // emitting the lent table publishes it without a copy
    step->outputs[0].type = SPC_DATA_SIGNAL;
    step->outputs[0].table = out;
    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
