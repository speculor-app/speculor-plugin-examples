cmake_minimum_required(VERSION 3.24)
project(SpeculorPluginExamples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Speculor SDK bundle is fully self-contained: it ships its own OpenCV,
# FFmpeg, Vulkan and spclib, plus the CMake config + helpers below. Just
# download a release from
#   https://github.com/speculor-app/speculor-sdk-dist/releases
# extract it, and point CMAKE_PREFIX_PATH at the extracted folder:
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-<version>-...
find_package(SpeculorSDK REQUIRED)
include(PluginHelpers)      # spc_add_plugin / spc_enable_gpu / spc_add_gpu_shaders
include(CompilerWarnings)   # spc_set_warnings

# All built plugins land in build/plugins/ — copy these into Speculor's plugins/.
set(SPC_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)

# ── Examples (real plugins, simplest → most complex) ─────────────────
# Pure SDK — no external dependencies:
add_subdirectory(examples/scalar_source)     # SCALAR out
add_subdirectory(examples/pattern_source)    # FRAME out (test pattern)
add_subdirectory(examples/wave_gen)          # SIGNAL out (synthetic waveform)
add_subdirectory(examples/logic_gate)        # SCALAR in -> SCALAR + CONTROL_MSG out
add_subdirectory(examples/system_stats)      # TABLE + RECORD out (OS metrics)

# SDK + bundled helper libs (pocketfft / OpenCV / spclib):
add_subdirectory(examples/audio_analyzer)    # SIGNAL in -> TABLE out (FFT, pocketfft)
add_subdirectory(examples/blob_detect)       # FRAME in -> TABLE out (spclib)
add_subdirectory(examples/bbox_display)      # TABLE + FRAME in -> FRAME out (OpenCV)

# GPU (Vulkan compute, with automatic CPU fallback):
add_subdirectory(examples/wmv_bgs)           # FRAME in -> FRAME out (GPU + spclib)
