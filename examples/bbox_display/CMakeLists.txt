spc_add_plugin(bbox_display SOURCES bbox_display_plugin.cpp)
target_link_libraries(bbox_display PRIVATE ${OpenCV_LIBS})
target_include_directories(bbox_display SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
