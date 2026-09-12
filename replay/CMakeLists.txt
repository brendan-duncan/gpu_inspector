cmake_minimum_required(VERSION 3.20)
project(GpuInspectorReplay CXX)

# vkinsp_replay: re-executes a .gpucap capture on this machine's GPU (docs/REPLAY.md). The JSON
# the layer writes is decoded by code generated from vk.xml (tools/gen_replay.py), and the Vulkan
# loader is opened at run time, so the headers may be newer than the installed loader.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(VKINSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VULKAN_HEADERS "${VKINSP_ROOT}/third_party/Vulkan-Headers")
set(VK_XML "${VULKAN_HEADERS}/registry/vk.xml")
set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")
set(GEN_SCRIPT "${VKINSP_ROOT}/tools/gen_replay.py")
set(GEN_OUTPUTS
    "${GEN_DIR}/vk_decode.gen.h"
    "${GEN_DIR}/vk_decode.gen.cpp")

add_custom_command(
    OUTPUT ${GEN_OUTPUTS}
    COMMAND ${Python3_EXECUTABLE} "${GEN_SCRIPT}" --xml "${VK_XML}" --out "${GEN_DIR}"
    DEPENDS "${GEN_SCRIPT}" "${VK_XML}"
            "${VKINSP_ROOT}/tools/vkgen/registry.py"
            "${VKINSP_ROOT}/tools/vkgen/serialize.py"
            "${VKINSP_ROOT}/tools/vkgen/deserialize.py"
            "${VKINSP_ROOT}/tools/vkgen/dispatch.py"
    COMMENT "Generating the replay's Vulkan decoders from vk.xml"
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
    src/history.cpp
    src/draw_stats.cpp
    src/pipeline_copy.cpp
    src/util.h
    ${GEN_OUTPUTS})

# format_info.h (bytes per texel) is shared with the capture layer.
target_include_directories(vkinsp_replay PRIVATE
    "${VULKAN_HEADERS}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GEN_DIR}"
    "${VKINSP_ROOT}/layer/src")

target_compile_definitions(vkinsp_replay PRIVATE VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(vkinsp_replay PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vkinsp_replay PRIVATE /W3 /bigobj)
else()
    target_compile_options(vkinsp_replay PRIVATE -Wall -Wno-unused-parameter)
    target_link_libraries(vkinsp_replay PRIVATE ${CMAKE_DL_LIBS})
endif()

set_target_properties(vkinsp_replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
