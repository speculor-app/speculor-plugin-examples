spc_add_plugin(system_stats SOURCES
    system_stats_plugin.cpp
    system_stats_sampling.cpp
)

if(WIN32)
    target_link_libraries(system_stats PRIVATE iphlpapi psapi)
elseif(APPLE)
    find_library(CORE_FOUNDATION CoreFoundation)
    find_library(IOKIT IOKit)
    target_link_libraries(system_stats PRIVATE ${CORE_FOUNDATION} ${IOKIT})
endif()
