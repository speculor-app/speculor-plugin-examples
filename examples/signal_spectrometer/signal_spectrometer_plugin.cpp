// signal_spectrometer — Renders a frequency spectrum from a real-valued signal.
//
// Demonstrates:
//   - Signal input with float amplitude schema
//   - Ring buffer for lock-free signal transfer
//   - Real-to-complex FFT (pocketfft r2c)
//   - Simpler than iq_spectrometer — single-channel real signal

#include <speculor/plugin_helpers.h>
#include <speculor/ring_buffer.h>
#include <pocketfft_hdronly.h>
#include <vector>
#include <cmath>
#include <cstring>

static constexpr uint32_t FFT_SIZE = 512;
static constexpr uint32_t RING_CAPACITY = 32768;
static constexpr uint32_t WIDTH = FFT_SIZE / 2;  // real FFT gives N/2+1 bins
static constexpr uint32_t HEIGHT = 200;

struct SignalSpecState {
    spc::HostServices host;
    SpcRingBuffer* ring = nullptr;

    std::vector<float> fft_in;
    std::vector<std::complex<float>> fft_out;
    std::vector<float> magnitude;

    std::vector<uint8_t> frame_buf;
    SpcFrame output_frame{};

    float floor_db = -60.0f;
    float range_db = 60.0f;
};

SPC_PLUGIN_CAST(SignalSpecState)
SPC_PLUGIN_HOST_SERVICES(SignalSpecState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("signal_spectrometer", "Signal Spectrometer", "Examples")
        .author("Speculor").version("1.0.0")
        .description("Renders a frequency spectrum from a real-valued signal input")
        .input_signal("signal", "Signal", {
            {"amplitude", SPC_FIELD_FLOAT},
        })
        .output("spectrum", "Spectrum", SPC_DATA_FRAME)
        .float_param("floor_db", "Floor (dB)", -120.0f, 0.0f, -60.0f, 1.0f)
        .float_param("range_db", "Range (dB)", 10.0f, 120.0f, 60.0f, 1.0f)
        .streaming().frame_alloc()
        .build()
)

SPC_PLUGIN_AUTO_PARAMS(SignalSpecState,
    SPC_BIND_FLOAT(SignalSpecState, "floor_db", floor_db),
    SPC_BIND_FLOAT(SignalSpecState, "range_db", range_db)
)

// signal callback — runs on producer thread
static int on_signal_handler(SignalSpecState* s, uint32_t port_index,
                             const SpcTable* data) {
    if (port_index != 0 || !data || !data->data) return -1;
    spc_ring_write(s->ring, data->data, data->record_count);
    return 0;
}

SPC_PLUGIN_SIGNAL_CALLBACK(SignalSpecState, on_signal_handler)

static SpcPluginInstance* create_instance() {
    auto* s = new SignalSpecState{};
    s->ring = spc_ring_create(RING_CAPACITY, sizeof(float));
    s->fft_in.resize(FFT_SIZE);
    s->fft_out.resize(FFT_SIZE / 2 + 1);
    s->magnitude.resize(FFT_SIZE / 2 + 1);
    s->frame_buf.resize(WIDTH * HEIGHT * 3);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst) {
    auto* s = state(inst);
    if (s->ring) spc_ring_destroy(s->ring);
    delete s;
}

static int start(SpcPluginInstance* inst) {
    spc_ring_reset(state(inst)->ring);
    return 0;
}

static int stop(SpcPluginInstance*) { return 0; }

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    if (output_count < 1) return -1;

    // drain ring — keep the latest FFT_SIZE samples
    float drain[4096];
    uint32_t head = 0;
    for (;;) {
        uint32_t n = spc_ring_read(s->ring, drain, 4096);
        if (n == 0) break;
        for (uint32_t i = 0; i < n; ++i) {
            s->fft_in[head % FFT_SIZE] = drain[i];
            ++head;
        }
    }

    if (head == 0) {
        std::memset(s->frame_buf.data(), 0, s->frame_buf.size());
    } else {
        // apply Hann window
        for (uint32_t i = 0; i < FFT_SIZE; ++i) {
            float w = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * i / (FFT_SIZE - 1)));
            s->fft_in[i] *= w;
        }

        // real-to-complex FFT
        pocketfft::shape_t shape{FFT_SIZE};
        pocketfft::stride_t si{sizeof(float)};
        pocketfft::stride_t so{sizeof(std::complex<float>)};
        pocketfft::r2c(shape, si, so, {0},
                       pocketfft::FORWARD, s->fft_in.data(), s->fft_out.data(), 1.0f);

        // power spectrum in dB
        float inv_n = 1.0f / static_cast<float>(FFT_SIZE);
        uint32_t bins = FFT_SIZE / 2;
        for (uint32_t i = 0; i < bins; ++i) {
            float re = s->fft_out[i].real() * inv_n;
            float im = s->fft_out[i].imag() * inv_n;
            s->magnitude[i] = 10.0f * std::log10(re * re + im * im + 1e-20f);
        }

        // render
        uint8_t* fb = s->frame_buf.data();
        std::memset(fb, 0, s->frame_buf.size());
        uint32_t stride = WIDTH * 3;

        for (uint32_t x = 0; x < WIDTH; ++x) {
            float norm = (s->magnitude[x] - s->floor_db) / s->range_db;
            norm = std::clamp(norm, 0.0f, 1.0f);
            uint32_t bar_h = static_cast<uint32_t>(norm * HEIGHT);

            for (uint32_t y = HEIGHT - bar_h; y < HEIGHT; ++y) {
                uint8_t* px = fb + y * stride + x * 3;
                float t = static_cast<float>(HEIGHT - y) / HEIGHT;
                px[0] = static_cast<uint8_t>(std::min(t * 2.0f, 1.0f) * 255);
                px[1] = static_cast<uint8_t>(std::min((1.0f - t) * 2.0f, 1.0f) * 255);
                px[2] = 0;
            }
        }
    }

    SpcFrame* out = s->host.acquire_frame(0, WIDTH, HEIGHT, SPC_PIXEL_FORMAT_RGB24);
    if (out) {
        std::memcpy(out->data, s->frame_buf.data(), s->frame_buf.size());
        spc::set_frame_output(outputs[0], out);
    } else {
        std::memset(&s->output_frame, 0, sizeof(SpcFrame));
        s->output_frame.data = s->frame_buf.data();
        s->output_frame.width = WIDTH;
        s->output_frame.height = HEIGHT;
        s->output_frame.stride = WIDTH * 3;
        s->output_frame.format = SPC_PIXEL_FORMAT_RGB24;
        spc::set_frame_output(outputs[0], &s->output_frame);
    }
    return 0;
}

SPC_DECLARE_PLUGIN(get_descriptor, create_instance, destroy_instance,
                   set_parameter, get_parameter, process, start, stop,
                   set_host_services, on_signal)
