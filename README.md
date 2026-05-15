# Speculor Plugin Examples

Example plugins for the [Speculor](https://github.com/speculor-app/speculor-app) multi-camera tracking platform. Each example demonstrates a different aspect of the plugin API, from the simplest scalar output to image processing with OpenCV.

## Quick Start

1. **Download the SDK** from [Releases](https://github.com/speculor-app/speculor-sdk/releases) and extract it
2. **Build the examples:**

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-0.7.0-Windows-x64
cmake --build build
```

3. **Copy the built plugins** from `build/plugins/` into Speculor's `plugins/` directory

## Examples

| Example | Description | Concepts |
|---------|-------------|----------|
| **hello_scalar** | Outputs a constant value | State struct, `SPC_PLUGIN`, `SPC_PLUGIN_AUTO_PARAMS`, scalar output |
| **pattern_generator** | Generates a scrolling gradient | Frame output, pool allocation, custom parameters, FPS pacing |
| **brightness_filter** | Adjusts brightness/contrast | Frame input/output, OpenCV, `cv_helpers.h`, passthrough optimization |
| **frame_stats** | Computes image statistics | Multiple outputs, enum parameters, logging |
| **gpu_invert** | Inverts colors on GPU | Vulkan compute shader, `GpuPipelineBase`, CPU fallback, `.gpu_compute()` |
| **signal_spectrometer** | Real-valued signal spectrum | Signal input, `on_signal` callback, ring buffer, FFT, frame rendering |
| **iq_spectrometer** | I/Q complex signal spectrum | I/Q signal schema, complex FFT, signal metadata, `SPC_PLUGIN_VTABLE` with `.on_signal` handler |

## Plugin Structure

Every plugin follows this pattern:

```cpp
#include <speculor/plugin_helpers.h>

// 1. State struct — your per-instance data
struct MyState {
    spc::HostServices host;  // required for logging + frame allocation
    // ... your fields ...
};

// 2. Boilerplate — generates create/destroy/state cast/host services
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

// 6. Export vtable
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

## CMakeLists.txt for Your Plugin

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyPlugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(SpeculorSDK REQUIRED)
include(PluginHelpers)
include(CompilerWarnings)

set(SPC_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)

spc_add_plugin(my_plugin SOURCES my_plugin.cpp)

# If using OpenCV (for image processing plugins):
# target_link_libraries(my_plugin PRIVATE ${OpenCV_LIBS})

# If using spclib (for BGS, tracking, video I/O):
# target_link_libraries(my_plugin PRIVATE SpeculorSDK::spclib)
```

## Key APIs

| Header | Purpose |
|--------|---------|
| `<speculor/plugin_helpers.h>` | All macros, DescriptorBuilder, parameter helpers |
| `<speculor/plugin_api.h>` | C ABI structs (SpcPluginVTable, SpcHostServices) |
| `<speculor/data_types.h>` | SpcFrame, SpcData, SpcScalar, pixel formats |
| `<cv_helpers.h>` | OpenCV integration (frame_to_mat, cv_type_for_format) |

## Data Types

| Type | Use for |
|------|---------|
| `SPC_DATA_FRAME` | Video frames (SpcFrame with pixel data) |
| `SPC_DATA_SCALAR` | Numeric values (float, int, bool) |
| `SPC_DATA_TABLE` | Structured tabular data |
| `SPC_DATA_RECORD` | JSON metadata |

## License

These examples are released under the MIT License. Use them as a starting point for your own plugins.
