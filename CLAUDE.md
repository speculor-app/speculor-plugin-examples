# CLAUDE.md — Speculor Plugin Examples

## Workflow Rules

- **Never commit or push** unless the user explicitly asks you to.
- **Always update documentation** (CLAUDE.md, README.md) before committing. Keep example listings and SDK feature demonstrations current with code changes.

## Project Overview

Example plugins for the [Speculor](https://github.com/speculor-app/speculor-app) platform, demonstrating SDK features from the simplest scalar output to GPU compute shaders and signal processing. Built against the [Speculor SDK](https://github.com/speculor-app/speculor-sdk).

## Directory Structure

```
examples/
  hello_scalar/                   # Simplest plugin — scalar output + parameter binding
  pattern_generator/              # Frame output, pool allocation, FPS pacing
  image_filter/                   # Frame I/O, OpenCV integration, passthrough optimization
  multi_io/                       # Multiple inputs/outputs, enum params, OpenCV
  gpu_invert/                     # Vulkan compute shader, CPU fallback, GPU pipeline
    shaders/                      #   invert.comp
  signal_spectrometer/            # Signal input, ring buffer, FFT, frame rendering
  iq_spectrometer/                # I/Q signal input, field schema, complex FFT
```

## Build System

- **CMake 3.24+**, C++20 required
- **External dep**: `find_package(SpeculorSDK REQUIRED)` — provides `PluginHelpers` and `CompilerWarnings` modules
- **Dependencies** (vcpkg): OpenCV 4, FFmpeg, libjpeg-turbo (for spclib transitive deps)
- **Output**: `build/plugins/`

### Build Commands

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK
cmake --build build
```

Deploy built plugins to the Speculor application's `plugins/` directory.

## Naming Conventions

| Element          | Convention               | Example                         |
|------------------|--------------------------|---------------------------------|
| Example directories | snake_case            | `hello_scalar/`, `gpu_invert/`  |
| Main source file | `{name}_plugin.cpp`     | `hello_scalar_plugin.cpp`       |
| State struct     | PascalCase + `State`     | `HelloState`, `PatternGenState` |
| GPU pipeline     | `{name}_gpu_pipeline.h/cpp` | `gpu_invert_pipeline.h`    |
| Shader files     | snake_case `.comp`       | `invert.comp`                   |
| Classes          | PascalCase               | `InvertPipeline`                |
| Functions        | snake_case               | `process()`, `on_signal_handler()` |
| Member variables | snake_case + trailing `_`| `pipeline_`, `ctx_`             |
| Constants/macros | UPPER_SNAKE_CASE         | `SPC_HAS_VULKAN`, `RING_CAPACITY` |
| Namespaces       | lowercase                | `spc::`, `spc::gpu::`          |
| Headers          | `.h`                     | `gpu_invert_pipeline.h`        |
| Header guards    | `#pragma once`           |                                 |

## Code Style Rules

- **Namespace**: GPU pipeline code in `spc::gpu::`, shared helpers in `spc::`
- **No `using namespace std;`** in headers
- **No raw `new`/`delete`** — use RAII, `std::make_unique`, containers

## Examples by SDK Feature

| Example | SDK Features Demonstrated |
|---------|--------------------------|
| **hello_scalar** | `SPC_PLUGIN()`, `SPC_PLUGIN_DESCRIPTOR()`, `SPC_PLUGIN_AUTO_PARAMS()`, `SPC_DECLARE_PLUGIN_FILTER()`, scalar output |
| **pattern_generator** | Manual `create_instance()`/`destroy_instance()`, frame pool allocation (`host.acquire_frame()`), FPS pacing, fallback buffers, `.frame_alloc()` capability |
| **image_filter** | Frame input/output, OpenCV integration (`cv_helpers.h`), passthrough optimization, `start()`/`stop()` callbacks, `SPC_DECLARE_PLUGIN_SOURCE()` |
| **multi_io** | Multiple frame outputs, enum parameters, logging macros |
| **gpu_invert** | Vulkan compute shaders, `GpuPipelineBase`, `spc_enable_gpu()`, `spc_add_gpu_shaders()`, `#ifdef SPC_HAS_VULKAN` CPU fallback, push constants, SSBOs |
| **signal_spectrometer** | Signal input, `on_signal()` callback, `SpcRingBuffer` (lock-free SPSC), FFT (pocketfft), frame rendering, `SPC_DECLARE_PLUGIN()` full vtable |
| **iq_spectrometer** | I/Q signal input with custom field schema, `SPC_PLUGIN_SIGNAL_CALLBACK()`, complex FFT, signal metadata (`center_freq_hz`, `sample_rate_hz`) |

## Plugin State Macros

```cpp
SPC_PLUGIN(StateType, host_field)           // Auto create/destroy/state/set_host_services
SPC_PLUGIN_CAST(StateType)                  // Manual state casting only
SPC_PLUGIN_HOST_SERVICES(StateType, field)  // Manual host services injection
```

## Plugin Export Macros

```cpp
SPC_DECLARE_PLUGIN_FILTER(...)              // Stateless filter (no start/stop)
SPC_DECLARE_PLUGIN_SOURCE(...)              // Streaming source (with start/stop)
SPC_DECLARE_PLUGIN(...)                     // Full vtable (with signal handler)
```

## Key Files

| Purpose               | Path                                              |
|-----------------------|---------------------------------------------------|
| Root CMake            | `CMakeLists.txt`                                  |
| vcpkg manifest        | `vcpkg.json`                                      |
| Simplest example      | `examples/hello_scalar/hello_scalar_plugin.cpp`   |
| GPU example           | `examples/gpu_invert/gpu_invert_plugin.cpp`       |
| Signal example        | `examples/iq_spectrometer/iq_spectrometer_plugin.cpp` |
