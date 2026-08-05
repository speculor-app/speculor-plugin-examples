# Conformance for every example, and the pattern to copy into your own project.
#
# The shape that matters is one add_test per plugin rather than one test that
# loops: ctest then reports "conformance.blob_detect" instead of "the tests
# failed", and -R lets you rerun a single plugin.
#
# The list is explicit and filtered through if(TARGET). Examples self-skip when
# their dependencies are absent — wmv_bgs needs Vulkan — so a name that did not
# build should drop out of the test list, not fail the configure.

set(_examples
    scalar_source
    pattern_source
    wave_gen
    logic_gate
    system_stats
    threshold_event
    clock_probe
    uplink_sink
    audio_analyzer
    blob_detect
    bbox_display
    wmv_bgs
)

set(_present "")
foreach(_e ${_examples})
    if(TARGET ${_e})
        list(APPEND _present ${_e})
    endif()
endforeach()

if(NOT _present)
    message(STATUS "[tests] no example targets built — skipping conformance")
    return()
endif()

# Builds the runner against the SDK, resolving includes and libraries from the
# bundle and linking dl where dlopen needs it. Each PLUGINS entry becomes a
# build dependency, so the plugin is always newer than the test that loads it.
spc_add_plugin_test(spc_conformance
    SOURCES conformance_runner.cpp
    PLUGINS ${_present}
)

foreach(_e ${_present})
    # $<TARGET_FILE:...> resolves to the built module's real path, which is the
    # only thing the runner needs — it dlopens that path directly.
    add_test(NAME conformance.${_e}
             COMMAND spc_conformance $<TARGET_FILE:${_e}>)
    set_tests_properties(conformance.${_e} PROPERTIES TIMEOUT 120)
endforeach()

message(STATUS "[tests] conformance registered for: ${_present}")
