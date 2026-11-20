# CLAUDE.md — Speculor Plugin Examples

## Workflow Rules

- **Never commit or push** unless the user explicitly asks you to.
- **Every new example gets a conformance entry in `tests/CMakeLists.txt`.** The list there is explicit — an example nobody adds is never tested, which looks exactly like one that passes. These files are templates people copy, so a defect here propagates into other people's plugins.
- **Always update documentation** (CLAUDE.md, README.md) before committing. Keep the example listings and data-type coverage current with code changes.

## Project Overview

A curated set of **real** [Speculor](https://speculor.app) plugins, pulled from
Speculor's own plugin set and trimmed to the subset that builds against nothing
but the public SDK bundle. They collectively exercise every `SpcDataType` but
`SPC_DATA_PACKET` (frame, scalar, signal, table, JSON, control) plus GPU
compute, the event stream (`emit_event`), the host's disciplined clock
(`host->time`), and replay-safe / license-gated nodes — and two of them chain
into runnable pipelines (see README). Each one is written against the plugin
boundary itself, `speculor/plugin.h`, so a reader can match every function in
it to a slot of `SpcPlugin`. Built against the
[Speculor SDK bundle](https://github.com/speculor-app/speculor-sdk-dist/releases)
(source: private `speculor-sdk`), **SDK 0.58.0** or later (plugin ABI 58).

## Directory Structure

```
examples/
  scalar_source/      # SCALAR out — minimal plugin
  pattern_source/     # FRAME out — scrolling gradient test pattern, FPS pacing
  wave_gen/           # SIGNAL out — synthetic waveform, real-time pacing
  logic_gate/         # SCALAR in -> SCALAR + CONTROL out — boolean logic
  system_stats/       # TABLE + JSON out — cross-platform OS metrics
  threshold_event/    # SCALAR in -> SpcEvent — emit_event, BEGIN/END spans
  clock_probe/        # JSON out — host disciplined clock (host->time)
  uplink_sink/        # SCALAR in -> sink — live_only + license_tier
  udp_scalar_source/  # SCALAR out — source + interrupt, bounded socket wait
  audio_analyzer/     # SIGNAL in -> TABLE out — take_signal, FFT
  blob_detect/        # FRAME in -> TABLE out — spclib connected components
  bbox_display/       # TABLE + FRAME in -> FRAME out — OpenCV drawing
  wmv_bgs/            # FRAME in -> FRAME out — Vulkan compute BGS + CPU fallback
    shaders/          #   wmv_compute.comp, wmv_pack_mask.comp
tests/
  conformance_runner.cpp  # SDK conformance against one plugin named on argv[1]
  CMakeLists.txt          # one add_test per example
templates/
  standalone-plugin/      # complete project to copy — own project(), own test.
                          #   NOT an add_subdirectory child: CI configures it
                          #   separately, which is what proves a bundle is
                          #   self-contained. Keep it that way.
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
- **No raw `new`/`delete`** — use RAII, `std::make_unique`, containers. The one exception is the
  instance itself, which crosses the C boundary: `start()` allocates it with `new (std::nothrow)`
  and `stop()` `delete`s it.

## Examples by SDK Feature

| Example | SDK Features Demonstrated |
|---------|--------------------------|
| **scalar_source** | The boundary at its smallest: `start`/`stop`/`process`/`params_changed` written out and exported as `spc_plugin`, parameters by position, `SPC_DATA_SCALAR` output, `.run_on_change()` |
| **pattern_source** | Frames from the host's pool (`host->frame`), FPS pacing, a frame of the plugin's own when the host lends none |
| **wave_gen** | `output_signal()` schema, signal blocks filled in tables the host lends (`host->table`), ENUM values (`.choice.index`), real-time pacing, `spc::log_context` |
| **logic_gate** | Multiple non-blocking scalar inputs, BOOL/ENUM/STRING values, driving other nodes' params from an `SPC_DATA_CONTROL` output: commands go through the host (`host->control`, via `spc::ControlOut`), never a message the plugin builds |
| **system_stats** | `output_table()` filled in a lent table (`host->table`, `field_offsets`) **and** an `SPC_DATA_JSON` output, multi-file plugin, platform libs |
| **threshold_event** | `SpcEvent` fill + `host->emit_event`, `SPC_EVENT_BEGIN`/`END` spans + severity, `spc::input_scalar` |
| **clock_probe** | The disciplined clock (`host->time`, `SpcTimeInfo`), `SPC_DATA_JSON` output, interval pacing |
| **uplink_sink** | `.live_only()` (`SPC_NODE_LIVE_ONLY`, replay egress safety) + `.license_tier(SPC_LICENSE_PERSONAL)`, pure sink |
| **udp_scalar_source** | `.source()` (`SPC_NODE_SOURCE`, recordable/replayable ingress), two-phase shutdown (`interrupt`, then `stop`), bounded `poll`/`WSAPoll` wait so engine shutdown terminates, a `start()` that fails with a log line, no `params_changed` |
| **audio_analyzer** | `input_signal()`, `host->take_signal` in `process()`, signal fields read through `field_offsets`, FFT (`pocketfft_hdronly.h`), metrics table |
| **blob_detect** | `SpeculorSDK::spclib` (`connectedBlobDetection`), `output_table()` of bboxes, `.gpu_ok()` input (`SPC_INPUT_GPU_OK`), `host->ensure_cpu_data`, `param_rules` |
| **bbox_display** | Consuming a table (`input_table()`, `.optional_fields()`, reading through `field_offsets`), `host->release_frame` for a frame it does not emit, OpenCV drawing via `cv_helpers.h`, `${OpenCV_LIBS}` link |
| **wmv_bgs** | `record_gpu` into the engine's command buffer (`SPC_GPU_RECORDED`, the `SpcGpuPass` rings), `spc_enable_gpu`, `spc_add_gpu_shaders`, `GpuPipelineBase`, `#ifdef SPC_HAS_VULKAN` CPU path via spclib `WeightedMovingVariance` |

## Plugin Shape

The examples write the boundary in `speculor/plugin.h` out. They do not use the SDK's composition layer (`spc::PluginSlots`, `SPC_EXPORT_PLUGIN_SLOTS`, `SPC_PLUGIN` / `SPC_PLUGIN_STATE`, `create_instance`, `destroy_instance`, `set_host_services`, `spc::params_by_name`, `spc::HostServices`). Each exports `SpcPlugin`'s own slots with `SPC_EXPORT_PLUGIN` (`plugin.h`), which defines `spc_plugin` with `abi` set:

```cpp
SPC_EXPORT_PLUGIN(
    .info           = info,    // defined by SPC_PLUGIN_DESCRIPTOR
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
    // .interrupt / .record_gpu / .ui_input / .control_reply / .param_rules /
    // .list_options as needed — in SpcPlugin's declaration order: info, start,
    // stop, process, interrupt, params_changed, record_gpu, ui_input,
    // control_reply, param_rules, list_options
)
```

A slot only a Vulkan build has goes in as `SPC_IF_VULKAN(.record_gpu = record_gpu,)`, since no `#ifdef` can go inside the macro's arguments, and so does the GPU use the node info declares: `SPC_IF_VULKAN(.gpu_compute())` (see `wmv_bgs`). Every function of the host a running node gets is set, so an example calls `host->fn(host, ...)` without checking it first.

- `static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)` allocates the state, keeps `host` when the plugin calls it, takes the values and returns the instance, or null after logging why (undoing what it made: `stop()` is not called then). `static void stop(SpcPluginInstance*)` tears the instance down and frees it.
- **Parameters by position.** An `enum Param` of indices next to the declarations; with more than one parameter, a `Params` struct filled by `read_params(values)` in `start()` and `params_changed()`. A STRING value lives only during the call: copy it. The host holds every value and has checked it against the declaration, so the plugin never validates or answers for one. Never change a declared id, default or range: projects save them. A parameter greyed out by other values is a `param_rules` function of the values alone, indexed by the same enum (see `bbox_display`).
- **Host calls** go through the `SpcHost*` directly where that is one line (`host->table(host, port, rows)`); every function is set, so none is null-checked. SDK conveniences only where they do real work: `SPC_PLUGIN_DESCRIPTOR` / `DescriptorBuilder`, `spc::log_context` + `SPC_LOG_*`, `spc::ControlOut`, the table accessors, the GPU helpers. Say in one comment, where a convenience is first used, which host call it wraps.
- **Threads.** The engine calls an instance one call at a time (interrupt aside), so its state needs no lock. `interrupt` sets flags and cuts blocking I/O only. `param_rules` and `list_options` take no instance.

## Tables

A table or signal output comes from the host: `host->table(host, output, rows)` lends a zeroed table of that output port's schema, valid until `process()` returns. Fill it, set its header (`timestamp_ns`, `sequence`, and `sample_rate_hz` on a signal), and put it in the step's outputs; the host publishes it with no copy. A new one is lent every call. It may have its `record_count` lowered and must not be kept, resized or freed. Do not keep an `SpcTable` member with `spc_table_init` / `spc_table_resize`.

Every table carries `field_offsets`. On a lent table it has one entry per field of the output port. On an input table it has one entry per field the input port declares, in declaration order: the `input_table()` fields, then the `.optional_fields({...})` ones, whose entry is `SPC_NO_FIELD` when the producer sends no field of that id and type. Declare every field the plugin reads and index `field_offsets` with an enum in the same order; never call `spc_schema_field_offset` on an input table.

## CI

`.github/workflows/build.yml` downloads the latest published SDK bundle from
`speculor-sdk-dist`, builds all examples on Windows + Linux, and runs `ctest`.
It is the authoritative "does this still build against the deployed SDK" check.

The weekly drift run is what makes it useful: it builds against whatever bundle
is current, so an SDK release that breaks these examples surfaces here rather
than in someone else's project. Conformance extends that from "still compiles"
to "still loads and honours the ABI" — a header change can keep every example
compiling while breaking the `SpcPlugin` the host actually reads.

## Key Files

| Purpose          | Path                                            |
|------------------|-------------------------------------------------|
| Root CMake       | `CMakeLists.txt`                                |
| Simplest example | `examples/scalar_source/scalar_source_plugin.cpp` |
| GPU example      | `examples/wmv_bgs/wmv_bgs_plugin.cpp`           |
| Signal example   | `examples/audio_analyzer/audio_analyzer_plugin.cpp` |
| Event example    | `examples/threshold_event/threshold_event_plugin.cpp` |
| Clock example    | `examples/clock_probe/clock_probe_plugin.cpp`   |
| CI               | `.github/workflows/build.yml`                   |
