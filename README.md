# Speculor Plugin Examples

Real, buildable example plugins for [Speculor](https://speculor.app) — the
real-time multi-camera / signal-processing platform. Every example here is an
actual plugin pulled from Speculor's own plugin set, trimmed to the subset that
builds against nothing but the public SDK. Together they exercise **every
Speculor data type** but compressed packets (frames, scalars, signals, tables,
JSON, control commands) plus GPU compute, the event stream, the host's
disciplined clock, and replay-safe / license-gated nodes — and two of them
chain into complete pipelines you can wire up in the app.

Each example is written against the plugin boundary itself, the C header
`speculor/plugin.h`: every function in an example fills a slot of `SpcPlugin`,
and every call to the engine goes through the `SpcHost` the engine hands the
plugin. They need **SDK 0.58.0** or later (plugin ABI 58).

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
     -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-<version>-Linux-x86_64
   cmake --build build
   ```

   On Windows (from a Developer PowerShell / `vcvarsall x64` shell):

   ```powershell
   cmake -S . -B build -G Ninja `
     -DCMAKE_PREFIX_PATH=C:\path\to\SpeculorSDK-<version>-Windows-x64
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

## Starting your own plugin

Copy [`templates/standalone-plugin/`](templates/standalone-plugin) — a complete project with a plugin, a `CMakeLists.txt` and a conformance test.

Everything under `examples/` is a child of one parent CMake project, so those `CMakeLists.txt` files are one line each and are **not** what you would write on your own. The template is standalone, and CI configures it separately against the published bundle, so it also proves an extracted bundle can build a plugin with nothing else installed.

## Examples

Simplest first. The ⭐ rows form the pipelines described below.

| Example | In → Out | Data types | Dependencies | Concepts |
|---------|----------|-----------|--------------|----------|
| **scalar_source** | — → scalar | `SPC_DATA_SCALAR` | SDK only | The smallest plugin: every slot written out, parameters by position |
| **pattern_source** ⭐ | — → frame | `SPC_DATA_FRAME` | SDK only | Frames from the host's pool (`host->frame`), FPS pacing, test pattern |
| **wave_gen** | — → signal | `SPC_DATA_SIGNAL` | SDK only | Signal blocks in tables the host lends (`host->table`), real-time pacing, enum params |
| **logic_gate** | scalar ×4 → scalar + ctrl | `SPC_DATA_SCALAR`, `SPC_DATA_CONTROL` | SDK only | Multiple inputs, boolean logic, setting other nodes' parameters through the host (`host->control`, via `spc::ControlOut`) |
| **system_stats** | — → table + JSON | `SPC_DATA_TABLE`, `SPC_DATA_JSON` | OS libs | Lent table **and** JSON output, cross-platform sampling, a plugin in several files |
| **threshold_event** | scalar → *(event)* | `SPC_DATA_SCALAR`, `SpcEvent` | SDK only | Emitting `SpcEvent` BEGIN/END spans with severity via `host->emit_event` |
| **clock_probe** | — → JSON | `SPC_DATA_JSON` | SDK only | The host's disciplined clock (`host->time`) — UTC ns, sync source, lock state |
| **uplink_sink** | scalar → — | `SPC_DATA_SCALAR` | SDK only | `.live_only()` replay-safe egress + `.license_tier()` gating; pure sink |
| **udp_scalar_source** | *(UDP)* → scalar | `SPC_DATA_SCALAR` | SDK only | `.source()` for recordable ingress + two-phase shutdown (`interrupt`, then `stop`) with a **bounded** socket wait; a `start()` that can fail |
| **audio_analyzer** | signal → table | `SPC_DATA_SIGNAL` → `SPC_DATA_TABLE` | pocketfft (bundled) | Signal blocks taken in `process()` (`host->take_signal`), FFT, metrics table |
| **blob_detect** ⭐ | frame → table | `SPC_DATA_FRAME` → `SPC_DATA_TABLE` | `spclib` | Connected-component detection, bbox table output, frame passthrough, `param_rules` |
| **bbox_display** ⭐ | table + frame → frame | `SPC_DATA_TABLE` + `SPC_DATA_FRAME` | OpenCV (bundled) | Consuming a table with required and optional fields, OpenCV drawing via `cv_helpers.h` |
| **wmv_bgs** ⭐ | frame → frame | `SPC_DATA_FRAME` | `spclib` + GPU | **Vulkan compute** background subtraction in `record_gpu`, with a CPU path in `process()` |

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

Speculor connects nodes with a small set of typed data (`SpcDataType` in
`<speculor/plugin.h>`). This set demonstrates all of them but `SPC_DATA_PACKET`
(compressed video passthrough):

| Type | Use for | Produced by | Consumed by |
|------|---------|-------------|-------------|
| `SPC_DATA_FRAME` | Video / image frames | pattern_source, blob_detect, bbox_display, wmv_bgs | wmv_bgs, blob_detect, bbox_display |
| `SPC_DATA_SCALAR` | Single typed numeric/bool values | scalar_source, logic_gate | logic_gate, threshold_event, uplink_sink |
| `SPC_DATA_SIGNAL` | High-throughput sample streams (audio / RF) | wave_gen | audio_analyzer |
| `SPC_DATA_TABLE` | Schema-described binary tables (hot path) | system_stats, audio_analyzer, blob_detect | bbox_display |
| `SPC_DATA_JSON` | JSON metadata (cold path) | system_stats, clock_probe | — |
| `SPC_DATA_CONTROL` | Commands that set other nodes' parameters | logic_gate | (any node's params) |

> `SpcEvent` (emitted by **threshold_event**) is not a port data type — it is a
> host call (`host->emit_event(host, &event)`) that the engine routes to the
> session's `/events` stream and the GUI, so it has no input port to consume it.

## Plugin structure

Every example has the same shape: the boundary in `speculor/plugin.h`, written
out.

```cpp
#include <speculor/plugin.h>          // the boundary
#include <speculor/plugin_helpers.h>  // conveniences: DescriptorBuilder, log helpers, ...

#include <new>

// 1. Parameters by position: start() and params_changed() get every value in
//    one array, in declaration order.
enum Param : uint32_t { P_GAIN };

// 2. The node info (the `info` slot): ports and parameter declarations.
//    SPC_PLUGIN_DESCRIPTOR defines get_descriptor(), built once and kept.
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_plugin", "My Plugin", "Category")
        .author("You").version("1.0.0")
        .output("out", "Output", SPC_DATA_FRAME)
        .float_param("gain", "Gain", 0.0f, 10.0f, 1.0f, 0.1f)   // P_GAIN
)

// 3. An instance: whatever the node needs while it runs.
struct MyState {
    SpcHost* host;   // valid until stop() returns
    float gain;
};

static MyState* state(SpcPluginInstance* instance) {
    return reinterpret_cast<MyState*>(instance);
}

// 4. An instance exists only while its node runs.
static SpcPluginInstance* start(SpcHost* host, const SpcValue* values) {
    auto* s = new (std::nothrow) MyState{host, values[P_GAIN].f};
    return reinterpret_cast<SpcPluginInstance*>(s);   // null: the node does not run
}
static void stop(SpcPluginInstance* instance) {
    delete state(instance);
}
static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/) {
    state(instance)->gain = values[P_GAIN].f;
}

// 5. One step: one SpcData per declared input and output, in port order.
static int process(SpcPluginInstance* instance, SpcStep* step) {
    auto* s = state(instance);
    // ... read step->inputs, fill step->outputs, call s->host->fn(s->host, ...)
    return SPC_OK;
}

// 6. The one symbol the library exports. A slot left out is null.
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
```

**Lifecycle and threads.** `start(host, values)` makes an instance when the node
starts and returns it, or null after logging why; `stop(instance)` tears it down,
and nothing is called on it afterwards. The engine calls an instance one call at
a time, `process`, `params_changed` and the others on the node's own thread, so
its state needs no lock. The exception is `interrupt`, which comes from another
thread before `stop()`, while a `process()` may be running: it sets flags and
cuts blocking I/O, nothing more (see `udp_scalar_source`). `param_rules` and
`list_options` take no instance at all.

**Parameters.** The host holds every value: it checks a new one against the
declaration, saves it with the project and shows it. The plugin gets all of
them, in declaration order, as one `SpcValue` array in `start()`, and again in
`params_changed()` with bit *i* of `changed` set for each one that changed. A
value is in the member its declared type names: `.f` (FLOAT), `.i` (INT, BOOL),
`.choice.index` (ENUM), `.color` (COLOR), `.str.ptr` (STRING, the host's and
valid during the call only: copy it). Each example keeps an enum of positions
next to its declarations. A parameter that is greyed out depending on other
values is a `param_rules` function of the values alone (see `blob_detect`,
`bbox_display`). A plugin that reads a parameter only in `start()` may leave
`params_changed` null: the host keeps a value set while the node runs, and the
next `start()` gets it (see `udp_scalar_source`).

**Host calls.** Every call to the engine goes through the `SpcHost` that
`start()` was handed, with the host itself as the first argument:
`host->table(host, port, rows)`, `host->emit_event(host, &event)`. A function
the host does not provide is null, so check before calling. Where a call needs
work around it, an example uses an SDK convenience compiled into the plugin and
says where it first appears what call it wraps: `spc::log_context(host)` for
printf-style logging over `host->log`, `spc::ControlOut` over `host->control`,
the GPU helpers over `host->gpu` and the ring calls of the GPU pass.

**Tables.** A table or signal output comes from the host:
`host->table(host, output, rows)` lends a zeroed table of that output port's
schema, valid until `process()` returns, and putting it in the step's outputs
emits it with no copy. Its `field_offsets` say where each field of the port is,
in the order the node info declares them (see `system_stats`, `blob_detect`,
`wave_gen`). A table arriving on an input carries `field_offsets` too, one entry
per field that input port declares. A field the plugin can do without is
declared with `.optional_fields({...})` after the port, and its entry is
`SPC_NO_FIELD` when the producer does not send it (see `bbox_display`). A plugin
declares every field it reads and never looks one up by name.

**Signals.** A signal input's slot in the step is always empty: the host copies
each block the producer sends and holds it until the node takes it with
`host->take_signal(host, port)`, usually at the top of `process()` (see
`audio_analyzer`). Nothing of the consumer runs on the producer's thread.

**GPU.** A GPU plugin adds a Vulkan pipeline and shaders, and a `record_gpu`
slot that records into the engine's command buffer (see `wmv_bgs`).

**The SDK's composition layer.** `plugin_macros.h` also has `SPC_EXPORT_PLUGIN`,
which in C++ builds `spc_plugin` from another set of functions held in
`spc::PluginSlots` (`create_instance`, `set_host_services`, a per-name
`set_parameter` through `spc::params_by_name`). These examples write the
boundary out instead, so every function in them is a slot of `SpcPlugin`.

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

## Testing your plugin

`tests/` runs the SDK's conformance checks against every example, and is meant to be copied into your own project:

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-<version>-...
cmake --build build
ctest --test-dir build --output-on-failure
```

The harness ships in the bundle at `include/speculor_common/testing/`, so there is nothing extra to install. `spc_add_plugin_test` builds the runner and puts those headers on the include path:

```cmake
enable_testing()
spc_add_plugin_test(my_tests SOURCES conformance_runner.cpp PLUGINS my_plugin)
add_test(NAME conformance.my_plugin COMMAND my_tests $<TARGET_FILE:my_plugin>)
```

Register **one `add_test` per plugin** — ctest then names the plugin that broke instead of reporting that "the tests" failed, and `ctest -R conformance.my_plugin` reruns just that one.

`run_conformance()` loads the plugin the way the host does, through the exported `spc_plugin`, and checks it answers the ABI honestly: the `abi` number and the slots every plugin has (`info`, `start`, `stop`), the node info's shape, parameter ids unique and defaults inside their own ranges, `start()` and `stop()` twice in a row, `process()` on a worker thread returning once `interrupt()` has run, table outputs lent by the host only, and each parameter set to its own default.

That is the floor, not the goal. It proves your plugin loads and is ABI-honest — never that it works. For that, drive it with `FakeHost` (an `SpcHost` stand-in with a frame pool, lent tables, captured logs and a controllable clock) and assert on what `process()` produces. Hand `PluginUnderTest` the host with `set_host_services(fake.services())` before `start()`: `start()` gets it, as it gets the engine's.

## Key APIs

| Header | Purpose |
|--------|---------|
| `<speculor/plugin.h>` | The boundary, in C: `SpcPlugin`, `SpcNodeInfo`, `SpcHost`, `SpcStep`, `SpcData`, `SpcFrame`, `SpcTable`, `SpcEvent`, results and limits |
| `<speculor/plugin_helpers.h>` | C++ conveniences: `DescriptorBuilder` + `SPC_PLUGIN_DESCRIPTOR`, `spc::log_context` + `SPC_LOG_*`, `spc::ControlOut`, scalar helpers |
| `<speculor/table_helpers.h>` | Table field accessors (`spc_table_get_*`, `spc_table_set_*`) |
| `<cv_helpers.h>` | OpenCV ↔ `SpcFrame` zero-copy helpers |
| `<gpu/gpu_host.h>` | The device the host lends (`context_from_host`), the buffer behind a GPU handle |
| `<gpu/gpu_pipeline_base.h>` | Base class for Vulkan compute pipelines |
| `<pocketfft_hdronly.h>` | Header-only FFT |
| `<testing/conformance.h>` | `run_conformance()` — checks every plugin must pass |
| `<testing/plugin_under_test.h>` | `PluginUnderTest` — loads a built plugin through its exported `spc_plugin` |
| `<testing/fake_host.h>` | `FakeHost` — an `SpcHost` stand-in for driving a plugin |

## License

These examples are released under the **Apache License 2.0** (see `LICENSE` and
`NOTICE`) — the same license as Speculor's first-party plugins they're drawn
from. Use them as a starting point for your own plugins; your plugin is your own
work product. The SDK bundle they build against ships its own third-party
notices (`THIRD_PARTY_NOTICES.md`).
