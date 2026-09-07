cmake_minimum_required(VERSION 3.20)
project(VulkanInspectorLayer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(VKINSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VULKAN_HEADERS "${VKINSP_ROOT}/third_party/Vulkan-Headers")
set(VK_XML "${VULKAN_HEADERS}/registry/vk.xml")
set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")
set(GEN_SCRIPT "${VKINSP_ROOT}/tools/gen_vulkan.py")
set(GEN_OUTPUTS
    "${GEN_DIR}/vk_dispatch.gen.h"
    "${GEN_DIR}/vk_commands.gen.h"
    "${GEN_DIR}/vk_entry.gen.cpp"
    "${GEN_DIR}/vk_serialize.gen.h"
    "${GEN_DIR}/vk_serialize.gen.cpp"
    "${GEN_DIR}/vk_hooks.gen.h")

add_custom_command(
    OUTPUT ${GEN_OUTPUTS}
    COMMAND ${Python3_EXECUTABLE} "${GEN_SCRIPT}" --xml "${VK_XML}" --out "${GEN_DIR}"
    DEPENDS "${GEN_SCRIPT}" "${VK_XML}"
            "${VKINSP_ROOT}/tools/vkgen/registry.py"
            "${VKINSP_ROOT}/tools/vkgen/dispatch.py"
            "${VKINSP_ROOT}/tools/vkgen/serialize.py"
    COMMENT "Generating Vulkan layer dispatch code from vk.xml"
    VERBATIM)

set(LAYER_NAME "VK_LAYER_INSPECTOR_capture")
set(LAYER_LIB "VkLayer_inspector_capture")

add_library(${LAYER_LIB} SHARED
    src/layer.h
    src/layer.cpp
    src/json_writer.h
    src/json_parse.h
    src/command_recorder.h
    src/tracker.h
    src/tracker.cpp
    src/hooks.h
    src/hooks.cpp
    src/resources.h
    src/resources.cpp
    src/capture.h
    src/capture.cpp
    src/image_readback.h
    src/image_readback.cpp
    src/format_info.h
    src/transport.h
    src/transport.cpp
    $<$<BOOL:${WIN32}>:src/layer.def>
    ${GEN_OUTPUTS})

target_include_directories(${LAYER_LIB} PRIVATE
    "${VULKAN_HEADERS}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GEN_DIR}")

target_compile_definitions(${LAYER_LIB} PRIVATE VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(${LAYER_LIB} PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${LAYER_LIB} PRIVATE /W3 /bigobj)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(${LAYER_LIB} PRIVATE VK_USE_PLATFORM_XCB_KHR VK_USE_PLATFORM_XLIB_KHR VK_USE_PLATFORM_WAYLAND_KHR)
    target_compile_options(${LAYER_LIB} PRIVATE -Wall -Wno-unused-parameter)
    target_link_options(${LAYER_LIB} PRIVATE -Wl,--no-undefined)
    find_package(Threads REQUIRED)
    target_link_libraries(${LAYER_LIB} PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(${LAYER_LIB} PRIVATE ws2_32)
endif()

set_target_properties(${LAYER_LIB} PROPERTIES
    PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Layer manifest, placed next to the library for every configuration.
if(WIN32)
    set(LAYER_LIB_PATH ".\\\\${LAYER_LIB}.dll")
else()
    set(LAYER_LIB_PATH "./lib${LAYER_LIB}.so")
endif()

# Header version drives the api_version field.
file(STRINGS "${VULKAN_HEADERS}/include/vulkan/vulkan_core.h" VK_HEADER_VERSION_LINE
     REGEX "^#define VK_HEADER_VERSION [0-9]+")
string(REGEX MATCH "[0-9]+$" VK_HEADER_PATCH "${VK_HEADER_VERSION_LINE}")
set(LAYER_API_VERSION "1.4.${VK_HEADER_PATCH}")

set(MANIFEST_CONTENT "{
    \"file_format_version\": \"1.2.0\",
    \"layer\": {
        \"name\": \"${LAYER_NAME}\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"${LAYER_LIB_PATH}\",
        \"api_version\": \"${LAYER_API_VERSION}\",
        \"implementation_version\": \"1\",
        \"description\": \"Vulkan Inspector capture layer\",
        \"functions\": {
            \"vkNegotiateLoaderLayerInterfaceVersion\": \"vkNegotiateLoaderLayerInterfaceVersion\"
        },
        \"enable_environment\": { \"VKINSP_ENABLE\": \"1\" },
        \"disable_environment\": { \"VKINSP_DISABLE\": \"1\" }
    }
}
")
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:${LAYER_LIB}>/${LAYER_NAME}.json"
     CONTENT "${MANIFEST_CONTENT}")
