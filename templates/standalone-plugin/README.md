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

**Generated parameters.** `SPC_PLUGIN_AUTO_PARAMS` writes `set_parameter`/`get_parameter` from a binding list, so the bindings *are* the implementation and a parameter cannot be declared and then left unreadable. Use it when parameter changes are pure dispatch.

Write those two by hand when changing a parameter has to *do* something — reopen a device, resize a buffer, mark state dirty. Every example under `examples/` does it that way; [`blob_detect`](../../examples/blob_detect) is a good reference.

**Pool-first frame output.** `process()` asks the engine's frame pool first, because a pooled frame is written in place and the engine skips a copy. The pool can be exhausted, so there is a fallback buffer — returning an error there would drop frames under load.

**A conformance test, in the same project.** `conformance_runner.cpp` loads the built plugin through its exported `spc_plugin_vtable` and checks it answers the ABI honestly. Keep it: it is the cheapest possible check that your plugin is loadable, and it costs one file. It proves nothing about whether your filter is *correct* — for that, drive the plugin with `FakeHost` and assert on what `process()` produces.
