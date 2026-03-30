spc_add_plugin(frame_stats SOURCES frame_stats_plugin.cpp)
target_link_libraries(frame_stats PRIVATE ${OpenCV_LIBS})
