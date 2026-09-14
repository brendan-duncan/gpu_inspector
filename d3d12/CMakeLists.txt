# The Direct3D 12 capture library, its launcher and its shader tool (see README.md).
#
#   dxinsp_capture.dll  injected into the target by the launcher; hooks D3D12 and DXGI, speaks the
#                       inspector's protocol (the Vulkan layer's, byte for byte).
#   dxinsp_launch.exe   starts an application suspended, injects the library, resumes it.
#   dxinsp_shader.exe   disassembles and reflects DXBC/DXIL bytecode for the Inspect panel.
cmake_minimum_required(VERSION 3.20)
project(GpuInspectorD3D12Capture CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DXINSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")
set(MINHOOK_DIR "${DXINSP_ROOT}/third_party/minhook")
if(NOT EXISTS "${MINHOOK_DIR}/src/hook.c")
    message(FATAL_ERROR "third_party/minhook is empty: run `git submodule update --init` (MinHook hooks the D3D12 entry points)")
endif()

# The generated tables come from the Windows SDK headers (tools/gen_d3d12.py) and are committed,
# so a build needs neither Python nor a particular SDK to compile them; they are regenerated when
# the generator changes and a Windows SDK is found.
find_package(Python3 COMPONENTS Interpreter)
set(GEN_OUTPUTS "${GEN_DIR}/d3d12_enums.gen.h" "${GEN_DIR}/d3d12_enums.gen.cpp" "${GEN_DIR}/d3d12_vtables.gen.h")
if(Python3_FOUND)
    add_custom_command(
        OUTPUT ${GEN_OUTPUTS}
        COMMAND ${Python3_EXECUTABLE} "${DXINSP_ROOT}/tools/gen_d3d12.py" --out "${GEN_DIR}"
        DEPENDS "${DXINSP_ROOT}/tools/gen_d3d12.py"
        COMMENT "Generating D3D12 enum and vtable tables from the Windows SDK"
        VERBATIM)
endif()

add_library(minhook STATIC
    "${MINHOOK_DIR}/src/buffer.c"
    "${MINHOOK_DIR}/src/hook.c"
    "${MINHOOK_DIR}/src/trampoline.c"
    "${MINHOOK_DIR}/src/hde/hde64.c")
target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")
set_target_properties(minhook PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(dxinsp_capture SHARED
    src/common.h
    src/log.cpp
    src/hook.h
    src/hook.cpp
    src/transport.h
    src/transport.cpp
    src/stacktrace.h
    src/stacktrace.cpp
    src/tracker.h
    src/tracker.cpp
    src/json.h
    src/formats.h
    src/formats.cpp
    src/serialize.h
    src/serialize.cpp
    src/descriptors.h
    src/descriptors.cpp
    src/resources.h
    src/resources.cpp
    src/command_recorder.h
    src/capture.h
    src/capture.cpp
    src/image_readback.h
    src/image_readback.cpp
    src/validation.h
    src/validation.cpp
    src/shader_reflect.h
    src/shader_reflect.cpp
    src/shader_edit.h
    src/shader_edit.cpp
    src/device_info.h
    src/device_info.cpp
    src/hooks.h
    src/hooks_object.cpp
    src/hooks_device.cpp
    src/hooks_command_list.cpp
    src/hooks_queue.cpp
    src/ui_messages.h
    src/ui_messages.cpp
    src/main.cpp
    ${GEN_OUTPUTS})

# ../vulkan/src for json_writer.h and json_parse.h, header-only and API-neutral: the JSON they
# write and read is what the UI speaks whichever API produced it.
target_include_directories(dxinsp_capture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GEN_DIR}"
    "${DXINSP_ROOT}/vulkan/src")
target_compile_definitions(dxinsp_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(dxinsp_capture PRIVATE /W3 /bigobj)
target_link_libraries(dxinsp_capture PRIVATE minhook d3d12 dxgi dxguid ws2_32 dbghelp)
set_target_properties(dxinsp_capture PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(dxinsp_launch launcher/main.cpp)
target_compile_definitions(dxinsp_launch PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
set_target_properties(dxinsp_launch PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The shader tool shares the reflection code with the library; it needs the log and the generated
# enum tables the code refers to, nothing else of the library.
add_executable(dxinsp_shader
    shader_tool/main.cpp
    src/shader_reflect.cpp
    src/log.cpp
    "${GEN_DIR}/d3d12_enums.gen.cpp")
target_include_directories(dxinsp_shader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GEN_DIR}"
    "${DXINSP_ROOT}/vulkan/src")
target_compile_definitions(dxinsp_shader PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS DXINSP_SHADER_TOOL)
target_link_libraries(dxinsp_shader PRIVATE d3d12 dxgi dxguid)
set_target_properties(dxinsp_shader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
