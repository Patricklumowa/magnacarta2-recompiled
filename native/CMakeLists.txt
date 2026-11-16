# magnacarta2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(magnacarta2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(MAGNACARTA2_SOURCES
    src/main.cpp
    src/magnacarta2_app.cpp
    src/frame_pacing.cpp
    src/graphics_copy.cpp
    src/shader_capture.cpp
    src/math_polyfill.cpp
)

if(WIN32)
    add_executable(magnacarta2 WIN32 ${MAGNACARTA2_SOURCES})
else()
    add_executable(magnacarta2 ${MAGNACARTA2_SOURCES})
endif()

set(MC2_RUN_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Directory for the playable build")
set_target_properties(magnacarta2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")

rexglue_setup_target(magnacarta2 GPU_PLUGINS xenos)

option(MC2_ENABLE_DRAW_CAPTURE "Build opt-in guest draw/resource capture hooks" OFF)
option(MC2_BUILD_NATIVE_DRAW_PROBE "Build the offscreen native draw validation tool" OFF)
if(MC2_ENABLE_DRAW_CAPTURE)
    add_library(mc2_texture_decode STATIC src/texture_decode.cpp)
    target_include_directories(mc2_texture_decode PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(mc2_texture_decode PUBLIC rex::runtime)
    target_sources(magnacarta2 PRIVATE src/draw_capture.cpp)
    target_link_libraries(magnacarta2 PRIVATE mc2_texture_decode)
    add_executable(mc2_texture_decode_check EXCLUDE_FROM_ALL tests/texture_decode_check.cpp)
    target_link_libraries(mc2_texture_decode_check PRIVATE mc2_texture_decode)
    set_target_properties(mc2_texture_decode_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")
    add_executable(mc2_draw_capture_check EXCLUDE_FROM_ALL tests/draw_capture_check.cpp src/draw_capture.cpp)
    target_include_directories(mc2_draw_capture_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(mc2_draw_capture_check PRIVATE mc2_texture_decode)
    set_target_properties(mc2_draw_capture_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")
endif()
if(MC2_BUILD_NATIVE_DRAW_PROBE)
    include(cmake/NativeDrawProbe.cmake)
endif()

# VMX byte shuffles map directly to SSSE3 PSHUFB. Without this, SIMDe expands
# each shuffle into scalar byte operations throughout the generated game code.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
    target_compile_options(magnacarta2_recomp PRIVATE -mssse3)
endif()

add_executable(mc2_math_check EXCLUDE_FROM_ALL
    tests/math_polyfill_check.cpp src/math_polyfill.cpp)

add_executable(mc2_shader_capture_check EXCLUDE_FROM_ALL tests/shader_capture_check.cpp)
add_executable(mc2_shader_capture_hook_check EXCLUDE_FROM_ALL
    tests/shader_capture_hook_check.cpp src/shader_capture.cpp)
target_include_directories(mc2_shader_capture_hook_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mc2_shader_capture_hook_check PRIVATE rex::runtime)
set_target_properties(mc2_shader_capture_hook_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")

# The diagnostic header is installed by the frame-capture runtime patch.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rexglue-sdk/include/rex/graphics/guest_frame_csv.h")
    add_executable(mc2_frame_csv_check EXCLUDE_FROM_ALL tests/guest_frame_csv_check.cpp)
    target_include_directories(mc2_frame_csv_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rexglue-sdk/include")
endif()

add_executable(mc2_controller_check EXCLUDE_FROM_ALL tests/controller_poll_check.cpp)
target_link_libraries(mc2_controller_check PRIVATE rex::runtime)
set_target_properties(mc2_controller_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")

add_executable(mc2_fps_check EXCLUDE_FROM_ALL tests/frame_pacing_check.cpp src/frame_pacing.cpp)
target_include_directories(mc2_fps_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mc2_fps_check PRIVATE rex::runtime)
set_target_properties(mc2_fps_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")

# Compile the exact generated leaf once without SSSE3 as a reference. Keep it
# out of the game binary; do not maintain a handwritten copy of its behavior.
# Codegen may repartition functions across numbered files on a fresh checkout.
file(GLOB MC2_COPY_CANDIDATES "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/magnacarta2_recomp.*.cpp")
set(MC2_COPY_BEGIN -1)
foreach(MC2_COPY_FILE IN LISTS MC2_COPY_CANDIDATES)
    file(READ "${MC2_COPY_FILE}" MC2_COPY_SOURCE)
    string(FIND "${MC2_COPY_SOURCE}" "DEFINE_REX_FUNC(sub_82371D88) {" MC2_COPY_BEGIN)
    if(MC2_COPY_BEGIN GREATER_EQUAL 0)
        break()
    endif()
endforeach()
if(MC2_COPY_BEGIN LESS 0)
    message(FATAL_ERROR "Generated graphics-copy reference is missing. Run tools/build_native.py to generate it.")
endif()
string(SUBSTRING "${MC2_COPY_SOURCE}" ${MC2_COPY_BEGIN} -1 MC2_COPY_SOURCE)
string(FIND "${MC2_COPY_SOURCE}" "\nDEFINE_REX_FUNC(" MC2_COPY_END)
if(MC2_COPY_END GREATER_EQUAL 0)
    string(SUBSTRING "${MC2_COPY_SOURCE}" 0 ${MC2_COPY_END} MC2_COPY_SOURCE)
endif()
string(REPLACE "DEFINE_REX_FUNC(sub_82371D88)" "extern \"C\" REX_FUNC(mc2_copy_reference)" MC2_COPY_SOURCE "${MC2_COPY_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/graphics_copy_reference.cpp"
    "#include \"generated/default/magnacarta2_init.h\"\n${MC2_COPY_SOURCE}")
add_executable(mc2_graphics_check EXCLUDE_FROM_ALL
    tests/graphics_copy_check.cpp src/graphics_copy.cpp src/math_polyfill.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/graphics_copy_reference.cpp")
target_include_directories(mc2_graphics_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mc2_graphics_check PRIVATE magnacarta2_recomp rex::runtime)
set_target_properties(mc2_graphics_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")

# Exercise the actual SDK packet decoder with an observed-register-write sink.
set(MC2_GPU_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rexglue-sdk/src/graphics/command_processor.cpp")
if(EXISTS "${MC2_GPU_SOURCE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MC2_GPU_SOURCE}")
    file(READ "${MC2_GPU_SOURCE}" MC2_GPU_SOURCE_TEXT)
    set(MC2_GPU_CHECK_METHODS "")
    foreach(METHOD "bool CommandProcessor::ExecutePacketType0(" "void CommandProcessor::WriteRegisterRangeFromRing(")
        string(FIND "${MC2_GPU_SOURCE_TEXT}" "${METHOD}" METHOD_BEGIN)
        string(SUBSTRING "${MC2_GPU_SOURCE_TEXT}" ${METHOD_BEGIN} -1 METHOD_SOURCE)
        string(FIND "${METHOD_SOURCE}" "\n}" METHOD_END)
        math(EXPR METHOD_END "${METHOD_END} + 2")
        string(SUBSTRING "${METHOD_SOURCE}" 0 ${METHOD_END} METHOD_SOURCE)
        string(APPEND MC2_GPU_CHECK_METHODS "${METHOD_SOURCE}\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gpu_register_methods.inc" "${MC2_GPU_CHECK_METHODS}")
    add_executable(mc2_gpu_register_check EXCLUDE_FROM_ALL tests/gpu_register_check.cpp)
    set(MC2_SAMPLE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rexglue-sdk/src/graphics/pipeline/shader/dxbc_translator_om.cpp")
    file(READ "${MC2_SAMPLE_SOURCE}" MC2_SAMPLE_SOURCE_TEXT)
    string(FIND "${MC2_SAMPLE_SOURCE_TEXT}" "int32_t next_offset =" MC2_SAMPLE_BEGIN)
    if(MC2_SAMPLE_BEGIN GREATER_EQUAL 0)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MC2_SAMPLE_SOURCE}")
        string(SUBSTRING "${MC2_SAMPLE_SOURCE_TEXT}" ${MC2_SAMPLE_BEGIN} -1 MC2_SAMPLE_EXPRESSION)
        string(FIND "${MC2_SAMPLE_EXPRESSION}" ";" MC2_SAMPLE_END)
        math(EXPR MC2_SAMPLE_END "${MC2_SAMPLE_END} + 1")
        string(SUBSTRING "${MC2_SAMPLE_EXPRESSION}" 0 ${MC2_SAMPLE_END} MC2_SAMPLE_EXPRESSION)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gpu_sample_offset.inc"
            "static int32_t SampleOffset(uint32_t i, uint32_t sample_count, uint32_t tile_width) {\n${MC2_SAMPLE_EXPRESSION}\nreturn next_offset;\n}\n")
        target_compile_definitions(mc2_gpu_register_check PRIVATE MC2_CHECK_SAMPLE_OFFSET)
    endif()
    target_include_directories(mc2_gpu_register_check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(mc2_gpu_register_check PRIVATE rex::runtime)
    set_target_properties(mc2_gpu_register_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MC2_RUN_DIR}")
endif()
