# Speculor Plugin Examples

Real, buildable example plugins for [Speculor](https://speculor.app) — the
real-time multi-camera / signal-processing platform. Every example here is an
actual plugin pulled from Speculor's own plugin set, trimmed to the subset that
builds against nothing but the public SDK. Together they exercise **every
Speculor data type** (frames, scalars, signals, tables, records, control
messages) plus GPU compute, and two of them chain into complete pipelines you
can wire up in the app.

## Quick Start

1. **Download the SDK bundle** from
   [speculor-sdk-dist releases](https://github.com/speculor-app/speculor-sdk-dist/releases)
   and extract it. The bundle is **self-contained** — it ships its own OpenCV,
   FFmpeg, Vulkan, spclib and the CMake config + helpers. You need nothing else
   beyond CMake (≥ 3.24), Ninja and a C++20 compiler. (No vcpkg, no system
   OpenCV/FFmpeg install.)

2. **Build the examples**, pointing `CMAKE_PREFIX_PATH` at the extracted folder:

   ```bash
   cmake -S . -B build -G Ninja \
     -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-0.13.1-Linux-x86_64
   cmake --build build
   ```

   On Windows (from a Developer PowerShell / `vcvarsall x64` shell):

   ```powershell
   cmake -S . -B build -G Ninja `
     -DCMAKE_PREFIX_PATH=C:\path\to\SpeculorSDK-0.13.1-Windows-x64
   cmake --build build
   ```

   > **Linux note:** examples that link `spclib` (here: `blob_detect`,
   > `bbox_display`, `wmv_bgs`) pull in the SDK's FFmpeg, which is built with
   > VA-API hardware acceleration. Install the VA-API/DRM system libraries
   > before building:
   > ```bash
   > sudo apt-get install -y libva-dev libdrm-dev
   > ```

3. **Copy the built plugins** from `build/plugins/` into Speculor's `plugins/`
   directory and restart the app — they'll appear in the node palette.

## Examples

Simplest first. The ⭐ rows form the pipelines described below.

| Example | In → Out | Data types | Dependencies | Concepts |
|---------|----------|-----------|--------------|----------|
| **scalar_source** | — → scalar | `SPC_DATA_SCALAR` | SDK only | Minimal plugin, `SPC_PLUGIN`, auto params |
| **pattern_source** ⭐ | — → frame | `SPC_DATA_FRAME` | SDK only | Frame-pool allocation, FPS pacing, test pattern |
| **wave_gen** | — → signal | `SPC_DATA_SIGNAL` | SDK only | High-throughput signal output, real-time pacing, enum params |
| **logic_gate** | scalar ×4 → scalar + ctrl | `SPC_DATA_SCALAR`, `SPC_DATA_CONTROL_MSG` | SDK only | Multiple inputs, boolean logic, emitting control messages to other nodes |
| **system_stats** | — → table + record | `SPC_DATA_TABLE`, `SPC_DATA_RECORD` | OS libs | Schema-described table **and** JSON record output, cross-platform sampling |
| **audio_analyzer** | signal → table | `SPC_DATA_SIGNAL` → `SPC_DATA_TABLE` | pocketfft (bundled) | `on_signal` callback, ring buffer, FFT, metrics table |
| **blob_detect** ⭐ | frame → table | `SPC_DATA_FRAME` → `SPC_DATA_TABLE` | `spclib` | Connected-component detection, bbox table output, frame passthrough |
| **bbox_display** ⭐ | table + frame → frame | `SPC_DATA_TABLE` + `SPC_DATA_FRAME` | OpenCV (bundled) | Consuming a table, OpenCV drawing via `cv_helpers.h` |
| **wmv_bgs** ⭐ | frame → frame | `SPC_DATA_FRAME` | `spclib` + GPU | **Vulkan compute** background subtraction with automatic CPU fallback |

### Pipelines you can build from these

**Motion detection (vision):**

```
pattern_source ─► wmv_bgs ─► blob_detect ─► bbox_display
   (frames)      (fg mask)    (boxes)     (annotated frames)
```

Drop a real camera/video source in place of `pattern_source` and you have a
working motion tracker built entirely from example plugins.

**Audio analysis (signal):**

```
wave_gen ─► audio_analyzer
 (signal)   (RMS / peak / centroid / dominant-freq table)
```

## Data types covered

Speculor connects nodes with a small set of typed packets (`SpcDataType` in
`<speculor/data_types.h>`). This set demonstrates all of them:

| Type | Use for | Produced by | Consumed by |
|------|---------|-------------|-------------|
| `SPC_DATA_FRAME` | Video / image frames | pattern_source, blob_detect, bbox_display, wmv_bgs | wmv_bgs, blob_detect, bbox_display |
| `SPC_DATA_SCALAR` | Single typed numeric/bool values | scalar_source, logic_gate | logic_gate |
| `SPC_DATA_SIGNAL` | High-throughput sample streams (audio / RF) | wave_gen | audio_analyzer |
| `SPC_DATA_TABLE` | Schema-described binary tables (hot path) | system_stats, audio_analyzer, blob_detect | bbox_display |
| `SPC_DATA_RECORD` | JSON metadata (cold path) | system_stats | — |
| `SPC_DATA_CONTROL_MSG` | Inter-node parameter control | logic_gate | (any node's params) |

## Plugin structure

Every plugin follows the same shape:

```cpp
#include <speculor/plugin_helpers.h>

// 1. State struct — your per-instance data
struct MyState {
    spc::HostServices host;  // logging + frame allocation
    float gain = 1.0f;
};

// 2. Boilerplate — generates create/destroy/state-cast/host-services
SPC_PLUGIN(MyState, host)

// 3. Descriptor — metadata, ports, parameters
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_plugin", "My Plugin", "Category")
        .author("You").version("1.0.0")
        .output("out", "Output", SPC_DATA_FRAME)
        .float_param("gain", "Gain", 0.0f, 10.0f, 1.0f, 0.1f)
        .build()
)

// 4. Parameters — auto-bind struct fields (or write manual set/get)
SPC_PLUGIN_AUTO_PARAMS(MyState,
    SPC_BIND_FLOAT(MyState, "gain", gain)
)

// 5. Process — called every tick
static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    // ... read inputs, write outputs ...
    return 0;
}

// 6. Export the vtable
SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .set_host_services = set_host_services
)
```

Signal-driven plugins add an `.on_signal` handler (see `audio_analyzer`); GPU
plugins add a Vulkan pipeline + shaders (see `wmv_bgs`).

## CMakeLists.txt for your own plugin

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyPlugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(SpeculorSDK REQUIRED)
include(PluginHelpers)
include(CompilerWarnings)

set(SPC_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)

spc_add_plugin(my_plugin SOURCES my_plugin.cpp)

# Image processing with OpenCV (bundled in the SDK):
# target_link_libraries(my_plugin PRIVATE ${OpenCV_LIBS})
# target_include_directories(my_plugin SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})

# Computer vision / background subtraction / tracking (bundled spclib):
# target_link_libraries(my_plugin PRIVATE SpeculorSDK::spclib)

# GPU compute (Vulkan) with shaders in ./shaders/*.comp:
# if(SPC_VULKAN_FOUND AND SPC_GLSLANG_VALIDATOR)
#     spc_enable_gpu(my_plugin)
#     spc_add_gpu_shaders(my_plugin SHADERS my_kernel.comp)
#     target_sources(my_plugin PRIVATE my_gpu_pipeline.h my_gpu_pipeline.cpp)
# endif()
```

`spc_add_plugin` links `SpeculorSDK::speculor_sdk` and adds the SDK's include
paths automatically. It does **not** auto-link OpenCV or spclib — opt in per
plugin as shown above.

## Key APIs

| Header | Purpose |
|--------|---------|
| `<speculor/plugin_helpers.h>` | Macros, `DescriptorBuilder`, parameter binding, logging |
| `<speculor/plugin_api.h>` | C ABI structs (`SpcPluginVTable`, `SpcHostServices`, `SpcData`) |
| `<speculor/data_types.h>` | `SpcFrame`, `SpcScalar`, `SpcTable`, `SpcRecord`, pixel formats |
| `<speculor/table_helpers.h>` | Schema table builders + field accessors |
| `<speculor/ring_buffer.h>` | Lock-free SPSC ring buffer (signal buffering) |
| `<cv_helpers.h>` | OpenCV ↔ `SpcFrame` zero-copy helpers |
| `<gpu/gpu_pipeline_base.h>` | Base class for Vulkan compute pipelines |
| `<pocketfft_hdronly.h>` | Header-only FFT |

## License

These examples are released under the **Apache License 2.0** (see `LICENSE` and
`NOTICE`) — the same license as Speculor's first-party plugins they're drawn
from. Use them as a starting point for your own plugins; your plugin is your own
work product. The SDK bundle they build against ships its own third-party
notices (`THIRD_PARTY_NOTICES.md`).
