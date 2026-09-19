cmake_minimum_required(VERSION 3.20)
project(GpuInspectorReplay CXX)

# vkinsp_replay: re-executes a .gpucap capture on this machine's GPU (docs/REPLAY.md). The JSON
# the layer writes is decoded by code generated from vk.xml (tools/gen_replay.py), which also
# generates the source emitters of Export to C++, and the Vulkan loader is opened at run time, so
# the headers may be newer than the installed loader.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(VKINSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(VULKAN_HEADERS "${VKINSP_ROOT}/third_party/Vulkan-Headers")
set(VK_XML "${VULKAN_HEADERS}/registry/vk.xml")
set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")
set(GEN_SCRIPT "${VKINSP_ROOT}/tools/gen_replay.py")
set(GEN_OUTPUTS
    "${GEN_DIR}/vk_decode.gen.h"
    "${GEN_DIR}/vk_decode.gen.cpp"
    "${GEN_DIR}/vk_emit.gen.h"
    "${GEN_DIR}/vk_emit.gen.cpp")

add_custom_command(
    OUTPUT ${GEN_OUTPUTS}
    COMMAND ${Python3_EXECUTABLE} "${GEN_SCRIPT}" --xml "${VK_XML}" --out "${GEN_DIR}"
    DEPENDS "${GEN_SCRIPT}" "${VK_XML}"
            "${VKINSP_ROOT}/tools/vkgen/registry.py"
            "${VKINSP_ROOT}/tools/vkgen/serialize.py"
            "${VKINSP_ROOT}/tools/vkgen/deserialize.py"
            "${VKINSP_ROOT}/tools/vkgen/emit_source.py"
            "${VKINSP_ROOT}/tools/vkgen/dispatch.py"
    COMMENT "Generating the replay's Vulkan decoders from vk.xml"
    VERBATIM)

# Export to C++ writes a project whose hand-written files (its support code and main) live in
# export_template as real sources, embedded into the tool as strings. The Vulkan headers this tool
# is built with go with them: the source is spelled with their names, and an SDK a year older lacks
# some (a promoted extension's struct), so the project carries the headers rather than needing an SDK.
set(EXPORT_TEMPLATE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/export_template")
set(EXPORT_TEMPLATES
    "${EXPORT_TEMPLATE_DIR}/main.cpp"
    "${EXPORT_TEMPLATE_DIR}/vk_support.h"
    "${EXPORT_TEMPLATE_DIR}/vk_support.cpp")
set(EXPORT_HEADERS
    "${VULKAN_HEADERS}/include/vulkan/vulkan.h"
    "${VULKAN_HEADERS}/include/vulkan/vulkan_core.h"
    "${VULKAN_HEADERS}/include/vulkan/vk_platform.h")
file(GLOB EXPORT_VIDEO_HEADERS "${VULKAN_HEADERS}/include/vk_video/*.h")
list(APPEND EXPORT_HEADERS ${EXPORT_VIDEO_HEADERS})
set(EXPORT_EMBEDDED ${EXPORT_TEMPLATES})
foreach(header ${EXPORT_HEADERS})
    file(RELATIVE_PATH header_name "${VULKAN_HEADERS}/include" "${header}")
    list(APPEND EXPORT_EMBEDDED "vulkan_headers/${header_name}=${header}")
endforeach()
set(EXPORT_TEMPLATES_CPP "${GEN_DIR}/export_templates.gen.cpp")
add_custom_command(
    OUTPUT "${EXPORT_TEMPLATES_CPP}"
    COMMAND ${Python3_EXECUTABLE} "${VKINSP_ROOT}/tools/embed_files.py" --out "${EXPORT_TEMPLATES_CPP}" --symbol kExportTemplates ${EXPORT_EMBEDDED}
    DEPENDS "${VKINSP_ROOT}/tools/embed_files.py" ${EXPORT_TEMPLATES} ${EXPORT_HEADERS}
    COMMENT "Embedding the exported project's support files"
    VERBATIM)

add_executable(vkinsp_replay
    src/main.cpp
    src/arena.h
    src/json.h
    src/json.cpp
    src/decode.h
    src/decode.cpp
    src/gpucap.h
    src/gpucap.cpp
    src/replayer.h
    src/replayer.cpp
    src/overdraw.cpp
    src/overlay.cpp
    src/mesh.cpp
    src/xfb_patch.h
    src/xfb_patch.cpp
    src/history.cpp
    src/draw_stats.cpp
    src/ablation.cpp
    src/hw_counters.h
    src/hw_counters.cpp
    src/nvperf.cpp
    src/pipeline_copy.cpp
    src/source_writer.h
    src/source_writer.cpp
    src/exporter.h
    src/exporter.cpp
    ${EXPORT_TEMPLATES_CPP}
    src/util.h
    ${GEN_OUTPUTS})

# format_info.h (bytes per texel) is shared with the capture layer.
target_include_directories(vkinsp_replay PRIVATE
    "${VULKAN_HEADERS}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GEN_DIR}"
    "${VKINSP_ROOT}/src/vulkan/src")

target_compile_definitions(vkinsp_replay PRIVATE VK_NO_PROTOTYPES)

# Hardware counters through NVIDIA's Nsight Perf SDK (docs/REPLAY.md, "Hardware counters"). The SDK's
# redistributable headers live in third_party/nvperf; its host library is loaded at run time, not
# linked, so nothing extra is needed to link. On by default when those headers are present; the
# portable VK_KHR_performance_query path is always compiled in.
set(NVPERF_DIR "${VKINSP_ROOT}/third_party/nvperf")
option(VKINSP_NVPERF "Build hardware counters with NVIDIA's Nsight Perf SDK" ON)
if(VKINSP_NVPERF AND EXISTS "${NVPERF_DIR}/NvPerfUtility/include/NvPerfVulkan.h")
    target_compile_definitions(vkinsp_replay PRIVATE VKINSP_NVPERF)
    target_include_directories(vkinsp_replay PRIVATE
        "${NVPERF_DIR}/NvPerfUtility/include"
        "${NVPERF_DIR}/include")
    message(STATUS "vkinsp_replay: hardware counters with the Nsight Perf SDK (third_party/nvperf)")
else()
    message(STATUS "vkinsp_replay: hardware counters via VK_KHR_performance_query only (no Nsight Perf SDK headers)")
endif()

if(WIN32)
    target_compile_definitions(vkinsp_replay PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vkinsp_replay PRIVATE /W3 /bigobj)
else()
    target_compile_options(vkinsp_replay PRIVATE -Wall -Wno-unused-parameter)
    target_link_libraries(vkinsp_replay PRIVATE ${CMAKE_DL_LIBS})
endif()

set_target_properties(vkinsp_replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
