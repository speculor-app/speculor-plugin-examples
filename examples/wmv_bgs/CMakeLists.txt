spc_add_plugin(wmv_bgs SOURCES wmv_bgs_plugin.cpp)
target_link_libraries(wmv_bgs PRIVATE SpeculorSDK::spclib)

# --- optional Vulkan GPU acceleration ---
if(SPC_VULKAN_FOUND AND SPC_GLSLANG_VALIDATOR)
    message(STATUS "WMV BGS: Vulkan found — building with GPU acceleration")
    spc_enable_gpu(wmv_bgs)
    spc_add_gpu_shaders(wmv_bgs SHADERS wmv_compute.comp wmv_pack_mask.comp)
    target_sources(wmv_bgs PRIVATE wmv_gpu_pipeline.h wmv_gpu_pipeline.cpp)
else()
    message(STATUS "WMV BGS: Vulkan not available — building CPU-only")
endif()
