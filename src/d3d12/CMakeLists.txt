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

set(DXINSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
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
    src/raytracing.h
    src/raytracing.cpp
    src/command_recorder.h
    src/capture.h
    src/capture.cpp
    src/image_readback.h
    src/image_readback.cpp
    src/hud.h
    src/hud.cpp
    src/validation.h
    src/cpu_timeline.cpp
    src/device_removed.cpp
    src/validation.cpp
    src/shader_reflect.h
    src/shader_reflect.cpp
    src/shader_edit.h
    src/shader_edit.cpp
    src/pass_record.h
    src/overdraw.h
    src/draw_overlay.cpp
    src/mesh_output.cpp
    src/overdraw.cpp
    src/pixel_history.cpp
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
# write and read is what the UI speaks whichever API produced it. The HUD's hud_text.h and the
# live pause's frame_pause.h are shared from there for the same reason -- neither has any graphics
# API in it, so the HUD reads the same and the pause behaves the same on all three backends.
target_include_directories(dxinsp_capture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # The HUD's DXBC, committed rather than compiled: a release build has no fxc
    # (tools/gen_hud_shaders.py).
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "${GEN_DIR}"
    "${DXINSP_ROOT}/src/vulkan/src")
target_compile_definitions(dxinsp_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(dxinsp_capture PRIVATE /W3 /bigobj)
# Debug information in every configuration, and a PDB beside the DLL. The library runs inside
# somebody else's process: when that process falls over, the only stack anybody gets is the one its
# own crash handler prints, and without this the frames in here are addresses. It costs nothing at
# run time -- the code is optimized as before, and the PDB is not loaded unless something reads it.
target_compile_options(dxinsp_capture PRIVATE $<$<NOT:$<CONFIG:Debug>>:/Zi>)
target_link_options(dxinsp_capture PRIVATE $<$<NOT:$<CONFIG:Debug>>:/DEBUG> $<$<NOT:$<CONFIG:Debug>>:/OPT:REF> $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)
target_link_libraries(dxinsp_capture PRIVATE minhook d3d12 dxgi dxguid ws2_32 iphlpapi dbghelp)
set_target_properties(dxinsp_capture PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(dxinsp_launch launcher/main.cpp)
target_compile_definitions(dxinsp_launch PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
# winmm: a millisecond timer while --watch polls for a process to start. psapi: EnumProcesses,
# which is a fraction of the cost of a toolhelp snapshot when a poll has to be that quick
# (launcher/main.cpp).
target_link_libraries(dxinsp_launch PRIVATE winmm psapi)
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
    "${DXINSP_ROOT}/src/vulkan/src")
target_compile_definitions(dxinsp_shader PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS DXINSP_SHADER_TOOL)
target_link_libraries(dxinsp_shader PRIVATE d3d12 dxgi dxguid)
set_target_properties(dxinsp_shader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# dxinsp_replay: re-executes a Direct3D 12 capture on this machine's GPU, and writes it out as a
# C++ project (replay/, docs/REPLAY.md "Direct3D 12"). The capture file, its JSON and the arena are
# the Vulkan replay's (src/replay/src), which have no graphics API in them; the format tables and
# the generated enum names are the capture library's. The exported project's hand-written files
# live in replay/export_template as real sources, embedded into the tool as strings.
if(Python3_Interpreter_FOUND)
    set(DXREPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/replay")
    set(VKREPLAY_SRC "${DXINSP_ROOT}/src/replay/src")
    set(DXREPLAY_TEMPLATES
        "${DXREPLAY_DIR}/export_template/main.cpp"
        "${DXREPLAY_DIR}/export_template/dx_support.h"
        "${DXREPLAY_DIR}/export_template/dx_support.cpp"
        # The window, shared with the Vulkan export (src/replay/export_template): Direct3D 12 needs the Win32 one only.
        "${DXINSP_ROOT}/src/replay/export_template/frame_window.h"
        "${DXINSP_ROOT}/src/replay/export_template/frame_window_win32.cpp")
    set(DXREPLAY_TEMPLATES_CPP "${CMAKE_CURRENT_BINARY_DIR}/dx_export_templates.gen.cpp")
    add_custom_command(
        OUTPUT "${DXREPLAY_TEMPLATES_CPP}"
        COMMAND ${Python3_EXECUTABLE} "${DXINSP_ROOT}/tools/embed_files.py" --out "${DXREPLAY_TEMPLATES_CPP}" --symbol kDxExportTemplates --namespace dxreplay ${DXREPLAY_TEMPLATES}
        DEPENDS "${DXINSP_ROOT}/tools/embed_files.py" ${DXREPLAY_TEMPLATES}
        COMMENT "Embedding the exported D3D12 project's support files"
        VERBATIM)
    add_executable(dxinsp_replay
        replay/src/main.cpp
        replay/src/dx_reflect.h
        replay/src/dx_decode.h
        replay/src/dx_decode.cpp
        replay/src/dx_source.h
        replay/src/dx_source.cpp
        replay/src/dx_replayer.h
        replay/src/dx_replayer.cpp
        replay/src/dx_raytracing.cpp
        replay/src/dx_exporter.h
        replay/src/dx_exporter.cpp
        replay/src/dx_counters.h
        replay/src/dx_counters.cpp
        replay/src/dx_nvperf.cpp
        replay/src/dx_measure.cpp
        "${VKREPLAY_SRC}/json.cpp"
        "${VKREPLAY_SRC}/gpucap.cpp"
        "${VKREPLAY_SRC}/decode.cpp"
        src/formats.cpp
        src/log.cpp
        "${GEN_DIR}/d3d12_enums.gen.cpp"
        "${DXREPLAY_TEMPLATES_CPP}")
    set_target_properties(dxinsp_replay PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    target_include_directories(dxinsp_replay PRIVATE
        "${DXREPLAY_DIR}/src"
        "${VKREPLAY_SRC}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${GEN_DIR}"
        "${DXINSP_ROOT}/src/vulkan/src")
    target_compile_definitions(dxinsp_replay PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(dxinsp_replay PRIVATE /W3 /bigobj)
    target_link_libraries(dxinsp_replay PRIVATE d3d12 dxgi dxguid)

    # Hardware counters through NVIDIA's Nsight Perf SDK, as vkinsp_replay has them: the
    # redistributable headers live in third_party/nvperf and its host library is loaded at run time
    # (dx_nvperf.cpp). The SDK ships a Vulkan range profiler but no D3D12 one, so only the core
    # headers and the API-neutral utility layer are used here.
    set(NVPERF_DIR "${DXINSP_ROOT}/third_party/nvperf")
    option(DXINSP_NVPERF "Build dxinsp_replay's hardware counters with NVIDIA's Nsight Perf SDK" ON)
    if(DXINSP_NVPERF AND EXISTS "${NVPERF_DIR}/include/nvperf_d3d12_target.h")
        target_compile_definitions(dxinsp_replay PRIVATE DXINSP_NVPERF)
        target_include_directories(dxinsp_replay PRIVATE
            "${NVPERF_DIR}/NvPerfUtility/include"
            "${NVPERF_DIR}/include")
        message(STATUS "dxinsp_replay: hardware counters with the Nsight Perf SDK (third_party/nvperf)")
    else()
        message(STATUS "dxinsp_replay: no hardware counters (the Nsight Perf SDK headers are not in third_party/nvperf)")
    endif()
endif()
