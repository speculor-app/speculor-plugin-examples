// Computes audio metrics (RMS, peak, crest factor, spectral centroid, dominant
// frequency, zero-crossing rate) over a sliding window of the samples arriving
// on a SIGNAL input, and outputs them as a one-row TABLE at a set rate.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>
#include <speculor/table_helpers.h>

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <new>
#include <numbers>
#include <thread>
#include <vector>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_WINDOW_SIZE,
    P_SAMPLE_RATE,
    P_UPDATE_RATE,
};

// the input ports, in declaration order
enum Input : uint32_t {
    IN_SIGNAL,
};

// signal_in field indices, in the order the input declares them
enum { F_IN_AMPLITUDE = 0 };

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

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("audio_analyzer", "Audio Analyzer", "Analysis/Audio")
        .author("Speculor").version("0.1.0")
        .help("Computes audio metrics: RMS, peak, crest factor, spectral centroid, dominant frequency, zero-crossing rate")
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
            .param_help("Number of samples per analysis window")
        .int_param("sample_rate", "Sample Rate", 8000, 192000, 44100, 1000, "Input")
            .param_help("Fallback sample rate if not provided by upstream signal")
        .float_param("update_rate", "Update Rate (Hz)", 1.0f, 60.0f, 10.0f, 1.0f, "Output")
            .param_help("Maximum analysis output rate in Hz")
)

struct Params
{
    int32_t window_size;
    int32_t sample_rate;
    float update_rate;
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.window_size = values[P_WINDOW_SIZE].i;
    p.sample_rate = values[P_SAMPLE_RATE].i;
    p.update_rate = values[P_UPDATE_RATE].f;
    return p;
}

struct AudioAnalyzerState
{
    SpcHost* host = nullptr;   // valid until stop() returns
    SpcLogContext log{};
    Params params{};

    // the newest samples, circular; sized once, in start()
    std::vector<float> window_buf;
    size_t window_pos = 0;
    bool fresh = false;        // samples arrived since the last output

    // Hann window (precomputed)
    std::vector<float> hann;

    // upstream metadata
    double upstream_sample_rate = 0.0;

    // timing
    std::chrono::steady_clock::time_point last_output_time;
    uint64_t sequence = 0;
};

static AudioAnalyzerState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<AudioAnalyzerState*>(instance);
}

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

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) AudioAnalyzerState{};
    if (!s) return nullptr;
    s->host = host;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    s->params = read_params(values);
    s->last_output_time = std::chrono::steady_clock::now();

    // the window is sized once: a new size takes effect at the next start
    auto ws = static_cast<size_t>(std::clamp(s->params.window_size, 256, 16384));
    s->window_buf.assign(ws, 0.0f);
    precompute_hann(s, ws);

    SPC_LOG_INFO(&s->log, "Audio Analyzer started (window=%d)", s->params.window_size);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    SPC_LOG_INFO(&s->log, "Audio Analyzer stopped (%llu frames output)",
                 static_cast<unsigned long long>(s->sequence));
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
    const size_t ws = s->window_buf.size();

    // The blocks sent to the signal input wait with the host, which copied
    // them, until the node takes them here, on its own thread. Each is valid
    // until the next take on the same port. (The input's slot in the step is
    // always SPC_DATA_NONE.)
    if (host->take_signal) {
        while (const SpcTable* block = host->take_signal(host, IN_SIGNAL)) {
            // where `amplitude` is in this producer's records: field_offsets
            // has one entry per field the input declares, and
            // spc_table_get_float (table_helpers.h) reads it there
            const uint32_t off = block->field_offsets[F_IN_AMPLITUDE];
            for (uint32_t i = 0; i < block->record_count; ++i) {
                s->window_buf[s->window_pos] = spc_table_get_float(block, i, off);
                s->window_pos = (s->window_pos + 1) % ws;
            }
            if (block->record_count > 0) s->fresh = true;
            if (block->sample_rate_hz > 0.0)
                s->upstream_sample_rate = block->sample_rate_hz;
        }
    }

    // rate limiting
    auto now = std::chrono::steady_clock::now();
    float rate = std::clamp(p.update_rate, 1.0f, 60.0f);
    auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(rate)));
    if (now < s->last_output_time + interval) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return SPC_OK;
    }

    if (!s->fresh) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return SPC_OK;
    }
    s->fresh = false;

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

    // one row in a table the host lends for output 0, valid until process() returns
    SpcTable* out = host->table ? host->table(host, 0, 1) : nullptr;
    if (!out) return SPC_FAILED;
    const uint32_t* off = out->field_offsets;

    spc_table_set_float(out, 0, off[F_RMS_DB], rms_db);
    spc_table_set_float(out, 0, off[F_PEAK_DB], peak_db);
    spc_table_set_float(out, 0, off[F_CREST_FACTOR], crest);
    spc_table_set_float(out, 0, off[F_SPECTRAL_CENTROID], spectral_centroid);
    spc_table_set_float(out, 0, off[F_DOMINANT_FREQ], dominant_freq);
    spc_table_set_float(out, 0, off[F_ZERO_CROSSING_RATE], zcr);

    out->sample_rate_hz = sr;
    out->sequence = s->sequence++;
    out->timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - s->last_output_time).count();

    s->last_output_time = now;

    step->outputs[0].type = SPC_DATA_TABLE;
    step->outputs[0].table = out;
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
};
}
