// iq_spectrometer — Renders a power spectrum from complex I/Q signal input.
//
// Demonstrates:
//   - Signal input port (.input_signal with I/Q field schema)
//   - on_signal callback (runs on producer thread, lock-free ring buffer)
//   - SPC_PLUGIN_SIGNAL_CALLBACK macro
//   - SPC_DECLARE_PLUGIN with signal handler (10th argument)
//   - Ring buffer for cross-thread signal transfer
//   - Simple FFT (pocketfft) for spectrum computation
//   - Frame output with pixel rendering

#include <speculor/plugin_helpers.h>
#include <speculor/ring_buffer.h>
#include <pocketfft_hdronly.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <complex>

static constexpr uint32_t FFT_SIZE = 1024;
static constexpr uint32_t RING_CAPACITY = 65536;
static constexpr uint32_t SPECTRUM_WIDTH = FFT_SIZE;
static constexpr uint32_t SPECTRUM_HEIGHT = 256;

struct IqSpectrometerState {
    spc::HostServices host;

    // signal ring: receives interleaved I/Q int16 pairs from on_signal
    SpcRingBuffer* iq_ring = nullptr;

    // FFT workspace
    std::vector<std::complex<float>> fft_in;
    std::vector<std::complex<float>> fft_out;
    std::vector<float> magnitude;

    // output frame buffer
    std::vector<uint8_t> frame_buf;
    SpcFrame output_frame{};

    // display config
    float floor_db = -80.0f;
    float range_db = 80.0f;

    // signal metadata
    double center_freq_hz = 0.0;
    double sample_rate_hz = 0.0;
};

SPC_PLUGIN_CAST(IqSpectrometerState)
SPC_PLUGIN_HOST_SERVICES(IqSpectrometerState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("iq_spectrometer", "I/Q Spectrometer", "Examples")
        .author("Speculor").version("1.0.0")
        .description("Renders a power spectrum from complex I/Q signal data")
        // I/Q signal input: each record is an interleaved I,Q int16 pair
        .input_signal("iq_in", "I/Q Signal", {
            {"i", SPC_FIELD_INT16},
            {"q", SPC_FIELD_INT16},
        })
        .output("spectrum", "Spectrum", SPC_DATA_FRAME)
        .float_param("floor_db", "Floor (dB)", -120.0f, 0.0f, -80.0f, 1.0f)
        .float_param("range_db", "Range (dB)", 10.0f, 120.0f, 80.0f, 1.0f)
        .streaming().frame_alloc()
        .build()
)

SPC_PLUGIN_AUTO_PARAMS(IqSpectrometerState,
    SPC_BIND_FLOAT(IqSpectrometerState, "floor_db", floor_db),
    SPC_BIND_FLOAT(IqSpectrometerState, "range_db", range_db)
)

// signal handler — called on the producer's thread, must be fast
static int on_signal_handler(IqSpectrometerState* s, uint32_t port_index,
                             const SpcTable* data) {
    if (port_index != 0 || !data || !data->data) return -1;

    // write raw I/Q samples into ring buffer (lock-free)
    // each record is 4 bytes: int16 I + int16 Q
    spc_ring_write(s->iq_ring, data->data, data->record_count);

    // capture signal metadata if available
    if (data->center_freq_hz > 0.0) s->center_freq_hz = data->center_freq_hz;
    if (data->sample_rate_hz > 0.0) s->sample_rate_hz = data->sample_rate_hz;
    return 0;
}

SPC_PLUGIN_SIGNAL_CALLBACK(IqSpectrometerState, on_signal_handler)

static SpcPluginInstance* create_instance() {
    auto* s = new IqSpectrometerState{};
    // ring stores I/Q pairs as int16+int16 = 4 bytes per element
    s->iq_ring = spc_ring_create(RING_CAPACITY, 4);
    s->fft_in.resize(FFT_SIZE);
    s->fft_out.resize(FFT_SIZE);
    s->magnitude.resize(FFT_SIZE);
    s->frame_buf.resize(SPECTRUM_WIDTH * SPECTRUM_HEIGHT * 3);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst) {
    auto* s = state(inst);
    if (s->iq_ring) spc_ring_destroy(s->iq_ring);
    delete s;
}

static int start(SpcPluginInstance* inst) {
    auto* s = state(inst);
    spc_ring_reset(s->iq_ring);
    SPC_LOG_INFO(&s->host.cached_log, "I/Q spectrometer started");
    return 0;
}

static int stop(SpcPluginInstance*) { return 0; }

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    if (output_count < 1) return -1;

    // drain ring buffer — grab the latest FFT_SIZE I/Q pairs
    struct IqPair { int16_t i; int16_t q; };
    IqPair drain[8192];

    // consume all available, keep only the latest FFT_SIZE
    uint32_t total = 0;
    for (;;) {
        uint32_t n = spc_ring_read(s->iq_ring, drain, 8192);
        if (n == 0) break;
        // copy last min(n, FFT_SIZE) into fft_in
        uint32_t start_idx = (n > FFT_SIZE) ? n - FFT_SIZE : 0;
        uint32_t copy_count = n - start_idx;
        uint32_t dest_offset = (total < FFT_SIZE) ? total : FFT_SIZE - copy_count;
        for (uint32_t j = 0; j < copy_count && dest_offset + j < FFT_SIZE; ++j) {
            auto& p = drain[start_idx + j];
            s->fft_in[dest_offset + j] = {
                static_cast<float>(p.i) / 32768.0f,
                static_cast<float>(p.q) / 32768.0f
            };
        }
        total += n;
    }

    if (total == 0) {
        // no new data — output blank frame
        std::memset(s->frame_buf.data(), 0, s->frame_buf.size());
    } else {
        // compute FFT using pocketfft
        pocketfft::shape_t shape{FFT_SIZE};
        pocketfft::stride_t stride_in{sizeof(std::complex<float>)};
        pocketfft::stride_t stride_out{sizeof(std::complex<float>)};
        pocketfft::c2c(shape, stride_in, stride_out, {0},
                       pocketfft::FORWARD, s->fft_in.data(), s->fft_out.data(), 1.0f);

        // compute power spectrum in dB (FFT-shifted: DC in center)
        float inv_n = 1.0f / static_cast<float>(FFT_SIZE);
        for (uint32_t i = 0; i < FFT_SIZE; ++i) {
            uint32_t shifted = (i + FFT_SIZE / 2) % FFT_SIZE;
            float re = s->fft_out[shifted].real() * inv_n;
            float im = s->fft_out[shifted].imag() * inv_n;
            float power = re * re + im * im;
            s->magnitude[i] = 10.0f * std::log10(power + 1e-20f);
        }

        // render spectrum into RGB frame
        uint8_t* fb = s->frame_buf.data();
        std::memset(fb, 0, s->frame_buf.size());
        uint32_t stride = SPECTRUM_WIDTH * 3;

        for (uint32_t x = 0; x < SPECTRUM_WIDTH; ++x) {
            // normalize magnitude to 0..1
            float norm = (s->magnitude[x] - s->floor_db) / s->range_db;
            norm = std::clamp(norm, 0.0f, 1.0f);

            // draw vertical bar from bottom
            uint32_t bar_h = static_cast<uint32_t>(norm * SPECTRUM_HEIGHT);
            for (uint32_t y = SPECTRUM_HEIGHT - bar_h; y < SPECTRUM_HEIGHT; ++y) {
                uint8_t* px = fb + y * stride + x * 3;
                // green→yellow→red color map
                float t = static_cast<float>(SPECTRUM_HEIGHT - y) / SPECTRUM_HEIGHT;
                px[0] = static_cast<uint8_t>(std::min(t * 2.0f, 1.0f) * 255);
                px[1] = static_cast<uint8_t>(std::min((1.0f - t) * 2.0f, 1.0f) * 255);
                px[2] = 0;
            }
        }
    }

    // output frame
    SpcFrame* out = s->host.acquire_frame(0, SPECTRUM_WIDTH, SPECTRUM_HEIGHT,
                                          SPC_PIXEL_FORMAT_RGB24);
    if (out) {
        std::memcpy(out->data, s->frame_buf.data(), s->frame_buf.size());
        out->frame_number = total;
        spc::set_frame_output(outputs[0], out);
    } else {
        std::memset(&s->output_frame, 0, sizeof(SpcFrame));
        s->output_frame.data = s->frame_buf.data();
        s->output_frame.width = SPECTRUM_WIDTH;
        s->output_frame.height = SPECTRUM_HEIGHT;
        s->output_frame.stride = SPECTRUM_WIDTH * 3;
        s->output_frame.format = SPC_PIXEL_FORMAT_RGB24;
        spc::set_frame_output(outputs[0], &s->output_frame);
    }
    return 0;
}

// export vtable with signal handler as 10th argument
SPC_DECLARE_PLUGIN(get_descriptor, create_instance, destroy_instance,
                   set_parameter, get_parameter, process, start, stop,
                   set_host_services, on_signal)
