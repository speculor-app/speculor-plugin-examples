cmake_minimum_required(VERSION 3.24)
project(SpeculorPluginExamples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find the Speculor SDK (download from GitHub Releases or build from source)
find_package(SpeculorSDK REQUIRED)
include(PluginHelpers)
include(CompilerWarnings)

set(SPC_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)

# ── Examples (simplest → most complex) ───────────────────────────
add_subdirectory(examples/hello_scalar)
add_subdirectory(examples/pattern_generator)
add_subdirectory(examples/image_filter)
add_subdirectory(examples/multi_io)
