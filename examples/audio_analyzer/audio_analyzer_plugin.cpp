#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>
#include <speculor/ring_buffer.h>

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <numbers>
#include <thread>
#include <vector>

// output schema field indices
enum {
    F_RMS_DB = 0,
    F_PEAK_DB,
    F_CREST_FACTOR,
    F_SPECTRAL_CENTROID,
    F_DOMINANT_FREQ,
    F_ZERO_CROSSING_RATE,
    OUT_FIELD_COUNT
};

constexpr size_t ring_capacity = 65536;

// ═══════════════════════════════════════════════════════════════════
// state
// ═══════════════════════════════════════════════════════════════════

// GUI-thread-set parameters, snapshotted on the worker (H6)
struct Params
{
    int32_t window_size = 4096;
    int32_t sample_rate = 44100;
    float update_rate = 10.0f;
};

struct AudioAnalyzerState
{
    spc::HostServices host;
    SpcTable output_table;
    uint32_t offsets[OUT_FIELD_COUNT];
    uint32_t stride;

    // signal input ring
    SpcRingBuffer* signal_ring = nullptr;

    // analysis window buffer
    std::vector<float> window_buf;
    size_t window_pos = 0;

    // Hann window (precomputed)
    std::vector<float> hann;

    // cross-thread parameter block (GUI writes, worker snapshots per frame)
    spc::SharedParams<Params> params;

    // upstream metadata
    double upstream_sample_rate = 0.0;

    // timing
    std::chrono::steady_clock::time_point last_output_time;
    uint64_t frame_number = 0;
};

SPC_PLUGIN_CAST(AudioAnalyzerState)
SPC_PLUGIN_HOST_SERVICES(AudioAnalyzerState, host)

// ═══════════════════════════════════════════════════════════════════
// signal handler (runs on producer thread)
// ═══════════════════════════════════════════════════════════════════

static int on_signal_handler(AudioAnalyzerState* s, uint32_t port_index, const SpcTable* data)
{
    if (port_index != 0) return -1;
    spc_ring_write(s->signal_ring, data->data, data->record_count);
    if (data->sample_rate_hz > 0.0)
        s->upstream_sample_rate = data->sample_rate_hz;
    return 0;
}

SPC_PLUGIN_SIGNAL_CALLBACK(AudioAnalyzerState, on_signal_handler)

// ═══════════════════════════════════════════════════════════════════
// Hann window precomputation
// ═══════════════════════════════════════════════════════════════════

static void precompute_hann(AudioAnalyzerState* s, size_t n)
{
    s->hann.resize(n);
    for (size_t i = 0; i < n; ++i)
        s->hann[i] = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float>
                                               * static_cast<float>(i)
                                               / static_cast<float>(n - 1)));
}

// round up to next power of 2
static size_t next_pow2(size_t v)
{
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// ═══════════════════════════════════════════════════════════════════
// descriptor
// ═══════════════════════════════════════════════════════════════════

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("audio_analyzer", "Audio Analyzer", "Analysis/Audio")
        .author("Speculor").version("0.1.0")
        .description("Computes audio metrics: RMS, peak, crest factor, spectral centroid, dominant frequency, zero-crossing rate")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"audio"})
        .input_signal("signal_in", "Signal In", {
            {"amplitude", SPC_FIELD_FLOAT},
        })
        .output_table("analysis_out", "Analysis", {
            {"rms_db",               SPC_FIELD_FLOAT},
            {"peak_db",              SPC_FIELD_FLOAT},
            {"crest_factor",         SPC_FIELD_FLOAT},
            {"spectral_centroid_hz", SPC_FIELD_FLOAT},
            {"dominant_freq_hz",     SPC_FIELD_FLOAT},
            {"zero_crossing_rate",   SPC_FIELD_FLOAT},
        })
        .int_param("window_size", "Window Size", 256, 16384, 4096, 256, "Analysis")
            .param_description("Number of samples per analysis window")
        .int_param("sample_rate", "Sample Rate", 8000, 192000, 44100, 1000, "Input")
            .param_description("Fallback sample rate if not provided by upstream signal")
        .float_param("update_rate", "Update Rate (Hz)", 1.0f, 60.0f, 10.0f, 1.0f, "Output")
            .param_description("Maximum analysis output rate in Hz")
        .streaming()
)

// ═══════════════════════════════════════════════════════════════════
// lifecycle
// ═══════════════════════════════════════════════════════════════════

static SpcPluginInstance* create_instance()
{
    auto* s = new AudioAnalyzerState{};

    auto* desc = get_descriptor();
    // output port is port index 1 (after input)
    spc_schema_compute_offsets(&desc->ports[1].schema, s->offsets, &s->stride);
    spc_table_init(&s->output_table, s->stride, &desc->ports[1].schema);

    s->signal_ring = spc_ring_create(ring_capacity, sizeof(float));
    s->window_buf.resize(4096, 0.0f);
    precompute_hann(s, 4096);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    spc_table_free(&s->output_table);
    spc_ring_destroy(s->signal_ring);
    delete s;
}

// ═══════════════════════════════════════════════════════════════════
// parameters
// ═══════════════════════════════════════════════════════════════════

static int set_parameter(SpcPluginInstance* inst, const char* name,
                         const SpcParameterDesc* value)
{
    bool matched = state(inst)->params.update([&](Params& p) {
        return spc::try_set_int  (name, value, "window_size", p.window_size)
            || spc::try_set_int  (name, value, "sample_rate", p.sample_rate)
            || spc::try_set_float(name, value, "update_rate", p.update_rate);
    });
    return matched ? 0 : -1;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                         SpcParameterDesc* out)
{
    const Params p = state(inst)->params.snapshot();
    if (spc::try_get_int  (name, out, "window_size", p.window_size)) return 0;
    if (spc::try_get_int  (name, out, "sample_rate", p.sample_rate)) return 0;
    if (spc::try_get_float(name, out, "update_rate", p.update_rate)) return 0;
    return -1;
}

// ═══════════════════════════════════════════════════════════════════
// streaming
// ═══════════════════════════════════════════════════════════════════

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    const Params p = s->params.snapshot();
    spc_ring_reset(s->signal_ring);
    s->window_pos = 0;
    s->frame_number = 0;
    s->upstream_sample_rate = 0.0;
    s->last_output_time = std::chrono::steady_clock::now();

    auto ws = static_cast<size_t>(std::clamp(p.window_size, 256, 16384));
    s->window_buf.assign(ws, 0.0f);
    precompute_hann(s, ws);

    SPC_LOG_INFO(&s->host.cached_log, "Audio Analyzer started (window=%d)", p.window_size);
    return 0;
}

static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    SPC_LOG_INFO(&s->host.cached_log, "Audio Analyzer stopped (%llu frames output)",
                 static_cast<unsigned long long>(s->frame_number));
    return 0;
}

// ═══════════════════════════════════════════════════════════════════
// process
// ═══════════════════════════════════════════════════════════════════

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/, uint32_t /*input_count*/,
                   SpcData* outputs, uint32_t output_count)
{
    auto* s = state(inst);
    if (output_count < 1) return -1;

    const Params p = s->params.snapshot();  // one consistent view per frame

    // rate limiting
    auto now = std::chrono::steady_clock::now();
    float rate = std::clamp(p.update_rate, 1.0f, 60.0f);
    auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(rate)));
    if (now < s->last_output_time + interval) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }

    float drain[8192];
    size_t ws = s->window_buf.size();
    bool window_ready = false;

    for (;;) {
        uint32_t n = spc_ring_read(s->signal_ring, drain, 8192);
        if (n == 0) break;
        for (uint32_t i = 0; i < n; ++i) {
            s->window_buf[s->window_pos] = drain[i];
            s->window_pos = (s->window_pos + 1) % ws;
        }
        window_ready = true;
    }

    if (!window_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 0;
    }

    double sr = (s->upstream_sample_rate > 0.0) ? s->upstream_sample_rate
                                                : static_cast<double>(p.sample_rate);

    // reorder window buffer so newest sample is last
    std::vector<float> samples(ws);
    for (size_t i = 0; i < ws; ++i)
        samples[i] = s->window_buf[(s->window_pos + i) % ws];

    float sum_sq = 0.0f;
    float peak = 0.0f;
    int zero_crossings = 0;

    for (size_t i = 0; i < ws; ++i) {
        float x = samples[i];
        sum_sq += x * x;
        float ax = std::abs(x);
        if (ax > peak) peak = ax;
        if (i > 0 && ((samples[i - 1] >= 0.0f) != (x >= 0.0f)))
            ++zero_crossings;
    }

    float rms = std::sqrt(sum_sq / static_cast<float>(ws));
    float rms_db = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -120.0f;
    float peak_db = (peak > 1e-10f) ? 20.0f * std::log10(peak) : -120.0f;
    float crest = (rms > 1e-10f) ? (peak / rms) : 0.0f;
    float zcr = static_cast<float>(zero_crossings) / static_cast<float>(ws);

    size_t fft_n = next_pow2(ws);
    std::vector<std::complex<float>> fft_buf(fft_n, {0.0f, 0.0f});
    for (size_t i = 0; i < ws; ++i)
        fft_buf[i] = {samples[i] * s->hann[i], 0.0f};

    pocketfft::shape_t shape{fft_n};
    pocketfft::stride_t stride{static_cast<ptrdiff_t>(sizeof(std::complex<float>))};
    pocketfft::c2c(shape, stride, stride, {0}, pocketfft::FORWARD,
                   fft_buf.data(), fft_buf.data(), 1.0f);

    float weighted_sum = 0.0f;
    float mag_sum = 0.0f;
    float max_mag = 0.0f;
    size_t max_bin = 0;
    size_t nyquist = fft_n / 2;
    float bin_resolution = static_cast<float>(sr) / static_cast<float>(fft_n);

    for (size_t k = 1; k < nyquist; ++k) {
        float mag = std::abs(fft_buf[k]);
        float freq = static_cast<float>(k) * bin_resolution;
        weighted_sum += freq * mag;
        mag_sum += mag;
        if (mag > max_mag) {
            max_mag = mag;
            max_bin = k;
        }
    }

    float spectral_centroid = (mag_sum > 1e-10f) ? (weighted_sum / mag_sum) : 0.0f;
    float dominant_freq = static_cast<float>(max_bin) * bin_resolution;

    if (spc_table_resize(&s->output_table, 1) != 0) return -1;

    spc_table_set_float(&s->output_table, 0, s->offsets[F_RMS_DB], rms_db);
    spc_table_set_float(&s->output_table, 0, s->offsets[F_PEAK_DB], peak_db);
    spc_table_set_float(&s->output_table, 0, s->offsets[F_CREST_FACTOR], crest);
    spc_table_set_float(&s->output_table, 0, s->offsets[F_SPECTRAL_CENTROID], spectral_centroid);
    spc_table_set_float(&s->output_table, 0, s->offsets[F_DOMINANT_FREQ], dominant_freq);
    spc_table_set_float(&s->output_table, 0, s->offsets[F_ZERO_CROSSING_RATE], zcr);

    s->output_table.sample_rate_hz = sr;
    s->output_table.frame_number = s->frame_number++;
    s->output_table.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - s->last_output_time).count();

    s->last_output_time = now;

    outputs[0].type = SPC_DATA_TABLE;
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
    .set_host_services = set_host_services,
    .on_signal         = on_signal
)
