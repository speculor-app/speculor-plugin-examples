# A complete standalone plugin project — the shape you start from.
#
# Deliberately NOT part of the parent build: it is configured on its own so it
# proves an extracted SDK bundle can build a plugin with nothing else installed.
# CI configures this directory separately for exactly that reason.
cmake_minimum_required(VERSION 3.24)
project(my_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolves from the extracted SDK bundle when CMAKE_PREFIX_PATH points at it,
# bringing in the SpeculorSDK:: imported targets.
find_package(SpeculorSDK REQUIRED)

# Both are required, and neither is optional. The config only appends its own
# directory to CMAKE_MODULE_PATH — it does not include these — so without
# PluginHelpers, spc_add_plugin is an unknown command; and spc_add_plugin itself
# calls spc_set_warnings, which lives in CompilerWarnings, so omitting the
# second fails inside the helper rather than at your call site.
include(PluginHelpers)
include(CompilerWarnings)

# Builds a correctly configured plugin shared library: no lib prefix, hidden
# visibility, RPATH wired for the bundled dependencies.
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)

# Conformance against the plugin you just built. Keep this — it is the cheapest
# check that your plugin is loadable and ABI-honest, and it costs one file.
option(SPECULOR_BUILD_TESTS "Build the plugin conformance test" ON)
if(SPECULOR_BUILD_TESTS)
    enable_testing()
    spc_add_plugin_test(my_plugin_tests
        SOURCES conformance_runner.cpp
        PLUGINS my_plugin
    )
    add_test(NAME conformance.my_plugin
             COMMAND my_plugin_tests $<TARGET_FILE:my_plugin>)
    set_tests_properties(conformance.my_plugin PROPERTIES TIMEOUT 120)
endif()
