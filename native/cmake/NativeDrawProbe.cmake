# Optional offline draw probe. No game data or generated shader blobs are built in.
if(NOT WIN32)
    message(FATAL_ERROR "The native draw probe currently requires Windows/D3D12")
endif()
get_filename_component(MC2_PROBE_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MC2_PLUME_ROOT "${MC2_PROBE_NATIVE_DIR}/../thirdparty/UnleashedRecomp/thirdparty/plume" CACHE PATH "Pinned plume checkout")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${MC2_PLUME_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE MC2_PLUME_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT MC2_PLUME_REVISION STREQUAL "11926860e878e68626ea99ec88562ce2b8badc4f")
    message(FATAL_ERROR "Native draw probe requires plume 11926860e878e68626ea99ec88562ce2b8badc4f")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${MC2_PLUME_ROOT}" apply --reverse --check
    "${MC2_PROBE_NATIVE_DIR}/patches/plume-1192686-readback.patch"
    RESULT_VARIABLE MC2_PLUME_PATCH_CHECK OUTPUT_QUIET ERROR_QUIET)
if(NOT MC2_PLUME_PATCH_CHECK EQUAL 0)
    message(FATAL_ERROR "Apply native/patches/plume-1192686-readback.patch to the pinned plume checkout before building the probe")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${MC2_PLUME_ROOT}" apply --reverse --check
    "${MC2_PROBE_NATIVE_DIR}/patches/plume-1192686-pipeline-validation.patch"
    RESULT_VARIABLE MC2_PLUME_PIPELINE_CHECK OUTPUT_QUIET ERROR_QUIET)
if(NOT MC2_PLUME_PIPELINE_CHECK EQUAL 0)
    message(FATAL_ERROR "Apply native/patches/plume-1192686-pipeline-validation.patch to the pinned plume checkout before building the probe")
endif()
# This experiment needs only D3D12, so do not pull Vulkan into the game build.
add_library(mc2_probe_plume STATIC
    "${MC2_PLUME_ROOT}/plume_d3d12.cpp"
    "${MC2_PLUME_ROOT}/contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
target_include_directories(mc2_probe_plume PUBLIC "${MC2_PLUME_ROOT}"
    "${MC2_PLUME_ROOT}/contrib/D3D12MemoryAllocator/include")
target_compile_features(mc2_probe_plume PUBLIC cxx_std_17)
target_compile_definitions(mc2_probe_plume PUBLIC NOMINMAX)
target_link_libraries(mc2_probe_plume PUBLIC d3d12 dxgi dxguid)
add_library(mc2_native_quad STATIC "${MC2_PROBE_NATIVE_DIR}/src/native_quad.cpp"
    "${MC2_PROBE_NATIVE_DIR}/src/native_device.cpp"
    "${MC2_PROBE_NATIVE_DIR}/src/native_vertex_layout.cpp"
    "${MC2_PROBE_NATIVE_DIR}/src/native_color_target.cpp"
    "${MC2_PROBE_NATIVE_DIR}/src/native_sampled_texture.cpp"
    "${MC2_PROBE_NATIVE_DIR}/src/native_depth.cpp")
target_include_directories(mc2_native_quad PUBLIC "${MC2_PROBE_NATIVE_DIR}/src")
target_link_libraries(mc2_native_quad PUBLIC mc2_probe_plume)
if(MC2_BUILD_NATIVE_DRAW_PROBE)
    add_executable(mc2_native_draw_probe "${MC2_PROBE_NATIVE_DIR}/src/native_draw_probe.cpp")
    target_compile_features(mc2_native_draw_probe PRIVATE cxx_std_17)
    target_link_libraries(mc2_native_draw_probe PRIVATE mc2_native_quad)
    add_executable(mc2_native_quad_check EXCLUDE_FROM_ALL "${MC2_PROBE_NATIVE_DIR}/tests/native_quad_check.cpp")
    target_link_libraries(mc2_native_quad_check PRIVATE mc2_native_quad)
    add_executable(mc2_native_depth_check EXCLUDE_FROM_ALL "${MC2_PROBE_NATIVE_DIR}/tests/native_depth_check.cpp")
    target_link_libraries(mc2_native_depth_check PRIVATE mc2_native_quad)
    add_executable(mc2_native_color_check EXCLUDE_FROM_ALL "${MC2_PROBE_NATIVE_DIR}/tests/native_color_check.cpp")
    target_link_libraries(mc2_native_color_check PRIVATE mc2_native_quad)
    add_executable(mc2_native_texture_check EXCLUDE_FROM_ALL "${MC2_PROBE_NATIVE_DIR}/tests/native_texture_check.cpp")
    target_link_libraries(mc2_native_texture_check PRIVATE mc2_native_quad)
endif()
