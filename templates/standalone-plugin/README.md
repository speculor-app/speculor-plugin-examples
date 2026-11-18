# Standalone plugin template

A complete, buildable plugin project. Copy this directory, rename `my_plugin`, and edit `process()`.

Everything under [`examples/`](../../examples) is a child of one parent CMake project, so those `CMakeLists.txt` files are a single line each — not what you would write on your own. This one is standalone: it has its own `project()` and finds the SDK through `CMAKE_PREFIX_PATH`, which is the shape a real plugin project takes.

## Build

Download and extract a bundle from [speculor-sdk-dist releases](https://github.com/speculor-app/speculor-sdk-dist/releases), then point `CMAKE_PREFIX_PATH` at the extracted folder:

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-<version>-Linux-x86_64
cmake --build build
ctest --test-dir build --output-on-failure
```

```powershell
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="C:\path\to\SpeculorSDK-<version>-Windows-x64"
cmake --build build
ctest --test-dir build --output-on-failure
```

The bundle is self-contained — it ships its own OpenCV, FFmpeg, Vulkan and spclib — so nothing else needs installing. This directory is configured separately in CI precisely to keep that true: if a bundle ever stopped being able to build a plugin on its own, this is what fails.

The built plugin lands in `build/` as `my_plugin.so` / `my_plugin.dll`. Drop it into Speculor's plugin directory to load it.

## What it shows

**The boundary, written out.** Each function in `my_plugin.cpp` fills a slot of `SpcPlugin` from `speculor/plugin.h`, and `spc_plugin` at the bottom is the one symbol the library exports: `start(host, values)` makes the instance when the node starts, `stop()` frees it, `process()` runs one step and `params_changed()` is told new values. To add a slot (`interrupt`, `record_gpu`, `param_rules`, ...), write its function and list it in `spc_plugin`, in `SpcPlugin`'s declaration order; the examples under [`examples/`](../../examples) show those three.

**Parameters by position.** `start()` and `params_changed()` get every value as one `SpcValue` array in declaration order, and `enum Param` names the positions. The host holds the values and checks each against its declaration, so there is nothing to write for reading or validating one. When changing a parameter has to *do* something — reopen a device, resize a buffer — do it in `params_changed()`, which runs on the node's thread between steps; [`blob_detect`](../../examples/blob_detect) is a good reference.

**Pool-first frame output.** `process()` asks the host for a pool frame first (`host->frame`), because it is written in place and published without a copy. When the host lends none, the plugin uses a buffer of its own, which the engine copies.

**A conformance test, in the same project.** `conformance_runner.cpp` loads the built plugin through its exported `spc_plugin` and checks it answers the ABI honestly. Keep it: it is the cheapest possible check that your plugin is loadable, and it costs one file. It proves nothing about whether your filter is *correct* — for that, drive the plugin with `FakeHost` and assert on what `process()` produces.

The template needs SDK 0.58.0 or later.
