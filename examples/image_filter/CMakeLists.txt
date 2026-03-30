spc_add_plugin(brightness_filter SOURCES brightness_filter_plugin.cpp)
target_link_libraries(brightness_filter PRIVATE ${OpenCV_LIBS})
