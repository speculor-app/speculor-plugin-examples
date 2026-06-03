spc_add_plugin(blob_detect SOURCES blob_detect_plugin.cpp)
target_link_libraries(blob_detect PRIVATE SpeculorSDK::spclib)
