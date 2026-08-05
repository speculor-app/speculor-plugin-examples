spc_add_plugin(udp_scalar_source SOURCES udp_scalar_source_plugin.cpp)

# Winsock is not linked by default; POSIX sockets need no extra library.
if(WIN32)
    target_link_libraries(udp_scalar_source PRIVATE ws2_32)
endif()
