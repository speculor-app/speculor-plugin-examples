# CLAUDE.md — Speculor Plugin Examples

## Workflow Rules

- **Never commit or push** unless the user explicitly asks you to.
- **Always update documentation** (CLAUDE.md, README.md) before committing. Keep the example listings and data-type coverage current with code changes.

## Project Overview

A curated set of **real** [Speculor](https://speculor.app) plugins, pulled from
Speculor's own plugin set and trimmed to the subset that builds against nothing
but the public SDK bundle. They collectively exercise every `SpcDataType`
(frame, scalar, signal, table, record, control message) plus GPU compute, and
two of them chain into runnable pipelines (see README). Built against the
[Speculor SDK bundle](https://github.com/speculor-app/speculor-sdk-dist/releases)
(source: private `speculor-sdk`).

## Directory Structure

```
examples/
  scalar_source/      # SCALAR out — minimal plugin
  pattern_source/     # FRAME out — scrolling gradient test pattern, FPS pacing
  wave_gen/           # SIGNAL out — synthetic waveform, real-time pacing
  logic_gate/         # SCALAR in -> SCALAR + CONTROL_MSG out — boolean logic
  system_stats/       # TABLE + RECORD out — cross-platform OS metrics
  audio_analyzer/     # SIGNAL in -> TABLE out — on_signal, ring buffer, FFT
  blob_detect/        # FRAME in -> TABLE out — spclib connected components
  bbox_display/       # TABLE + FRAME in -> FRAME out — OpenCV drawing
  wmv_bgs/            # FRAME in -> FRAME out — Vulkan compute BGS + CPU fallback
    shaders/          #   wmv_compute.comp, wmv_pack_mask.comp
```

## Build System

- **CMake 3.24+**, C++20 required.
- **Only dependency**: `find_package(SpeculorSDK REQUIRED)`. The SDK bundle is
  self-contained — it ships OpenCV, FFmpeg, Vulkan, spclib **and** the CMake
  config + helper modules (`PluginHelpers`, `CompilerWarnings`). There is **no
  `vcpkg.json`**; do not reintroduce one.
- **Output**: `build/plugins/` (set via `SPC_PLUGIN_OUTPUT_DIR`).

### Build Commands

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-<ver>-<platform>
cmake --build build
```

Download the bundle from
https://github.com/speculor-app/speculor-sdk-dist/releases and extract it; point
`CMAKE_PREFIX_PATH` at the extracted folder. Deploy built plugins to the Speculor
application's `plugins/` directory.

### What the bundle exposes to a plugin's CMake

| Provided | Use |
|----------|-----|
| `spc_add_plugin(name SOURCES ...)` | Defines the shared-lib plugin target; links `SpeculorSDK::speculor_sdk`, adds SDK includes, routes output to `SPC_PLUGIN_OUTPUT_DIR`. Does **not** auto-link OpenCV/spclib. |
| `${OpenCV_LIBS}` / `${OpenCV_INCLUDE_DIRS}` | Link/include OpenCV (image plugins). No `OpenCV::core`-style targets exist. |
| `SpeculorSDK::spclib` | CV library: BGS, blob detection, optical flow, SORT tracking, video I/O. |
| `SpeculorSDK::speculor_gpu` | Vulkan GPU runtime (linked via `spc_enable_gpu`). |
| `SPC_VULKAN_FOUND`, `SPC_GLSLANG_VALIDATOR` | Gate GPU builds; `spc_add_gpu_shaders` needs both. |
| `spc_enable_gpu(t)` / `spc_add_gpu_shaders(t SHADERS ...)` | Enable GPU + compile `shaders/*.comp` → embedded SPIR-V headers. |

## Naming Conventions

| Element             | Convention                  | Example                          |
|---------------------|-----------------------------|----------------------------------|
| Example directories | snake_case (= plugin id)    | `scalar_source/`, `wmv_bgs/`     |
| Main source file    | `{name}_plugin.cpp`         | `pattern_source_plugin.cpp`      |
| State struct        | PascalCase + `State`        | `WmvState`, `BlobState`          |
| GPU pipeline        | `{name}_gpu_pipeline.h/cpp` | `wmv_gpu_pipeline.h`             |
| Shader files        | snake_case `.comp`          | `wmv_compute.comp`               |
| Member variables    | snake_case + trailing `_`   | `pipeline_`, `ctx_`              |
| Constants/macros    | UPPER_SNAKE_CASE            | `SPC_HAS_VULKAN`                 |
| Namespaces          | lowercase                   | `spc::`, `spc::gpu::`            |
| Header guards       | `#pragma once`              |                                  |

## Code Style Rules

- **Namespace**: GPU pipeline code in `spc::gpu::`, shared helpers in `spc::`.
- **No `using namespace std;`** in headers.
- **No raw `new`/`delete`** — use RAII, `std::make_unique`, containers.

## Examples by SDK Feature

| Example | SDK Features Demonstrated |
|---------|--------------------------|
| **scalar_source** | `SPC_PLUGIN()`, `SPC_PLUGIN_DESCRIPTOR()`, auto params, `SPC_DATA_SCALAR` output |
| **pattern_source** | Frame-pool allocation (`host.acquire_frame()`), FPS pacing, fallback buffer |
| **wave_gen** | `output_signal()` schema, high-throughput signal output, enum params, real-time pacing |
| **logic_gate** | Multiple scalar inputs, boolean logic, emitting `SPC_DATA_CONTROL_MSG` to drive other nodes' params |
| **system_stats** | `output_table()` schema **and** `SPC_DATA_RECORD` JSON co-output, multi-file plugin, platform libs |
| **audio_analyzer** | `input_signal()`, `on_signal` callback, `SpcRingBuffer`, FFT (`pocketfft_hdronly.h`), metrics table |
| **blob_detect** | `SpeculorSDK::spclib` (`connectedBlobDetection`), `output_table()` of bboxes, `.passthrough()` input |
| **bbox_display** | Consuming a table (`input_table()`), OpenCV drawing via `cv_helpers.h`, `${OpenCV_LIBS}` link |
| **wmv_bgs** | Vulkan compute (`spc_enable_gpu`, `spc_add_gpu_shaders`), `GpuPipelineBase`, `#ifdef SPC_HAS_VULKAN` CPU fallback via spclib `WeightedMovingVariance` |

## Plugin State Macros

```cpp
SPC_PLUGIN(StateType, host_field)           // Auto create/destroy/state/set_host_services
SPC_PLUGIN_CAST(StateType)                  // Manual state casting only
SPC_PLUGIN_HOST_SERVICES(StateType, field)  // Manual host services injection
SPC_PLUGIN_SIGNAL_CALLBACK(StateType, fn)   // Wire up an on_signal handler
```

## Plugin Export

```cpp
SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .set_host_services = set_host_services,
    // add .start / .stop / .on_signal / .on_event / .record_gpu as needed —
    // fields must appear in struct-declaration order
)
```

## CI

`.github/workflows/build.yml` downloads the latest published SDK bundle from
`speculor-sdk-dist` and builds all examples on Windows + Linux. It is the
authoritative "does this still build against the deployed SDK" check.

## Key Files

| Purpose          | Path                                            |
|------------------|-------------------------------------------------|
| Root CMake       | `CMakeLists.txt`                                |
| Simplest example | `examples/scalar_source/scalar_source_plugin.cpp` |
| GPU example      | `examples/wmv_bgs/wmv_bgs_plugin.cpp`           |
| Signal example   | `examples/audio_analyzer/audio_analyzer_plugin.cpp` |
| CI               | `.github/workflows/build.yml`                   |
