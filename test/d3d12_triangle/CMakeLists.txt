# A small Direct3D 12 application that re-records its command list every frame: the counterpart
# of test/triangle, used to exercise the D3D12 capture library's object tracking and frame capture
# (command recording, render target readback) without a Unity build. Configures both as a
# subdirectory of the root project and on its own (cmake -S test/d3d12_triangle -B <dir>).
cmake_minimum_required(VERSION 3.20)
project(dxinsp_triangle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "dxinsp_triangle is a Direct3D 12 application and builds on Windows only")
endif()

# The shaders are compiled at build time with dxc, from the Vulkan SDK or a Windows SDK that ships
# it (10.0.20348 and newer keep one in bin/<version>/x64).
file(GLOB WINSDK_BIN_DIRS "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT WINSDK_BIN_DIRS ORDER DESCENDING)
find_program(DXC dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" ${WINSDK_BIN_DIRS})
if(NOT DXC)
    message(FATAL_ERROR "dxc not found (install the Vulkan SDK or set VULKAN_SDK; a recent Windows SDK also has it)")
endif()

set(OUT_DIR "${CMAKE_BINARY_DIR}/bin")

# One compile per entry point: source, profile, entry, output.
set(SHADERS
    "cube.hlsl|vs_6_0|VSMain|cube_vs.cso"
    "cube.hlsl|ps_6_0|PSMain|cube_ps.cso"
    "wave.hlsl|cs_6_0|CSMain|wave_cs.cso")
set(CSO_OUTPUTS)
foreach(spec ${SHADERS})
    string(REPLACE "|" ";" parts "${spec}")
    list(GET parts 0 src)
    list(GET parts 1 profile)
    list(GET parts 2 entry)
    list(GET parts 3 out)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
        # -Zi keeps the HLSL source and line information inside the DXIL container (the
        # inspector's Source view and line links); -Qembed_debug adds nothing to that, it only
        # silences dxc's warning about having no -Fd to write a PDB to instead.
        COMMAND ${DXC} -T ${profile} -E ${entry} -Zi -Qembed_debug
                -Fo "${CMAKE_CURRENT_BINARY_DIR}/${out}" "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
        COMMENT "Compiling ${src} (${entry})"
        VERBATIM)
    list(APPEND CSO_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/${out}")
endforeach()

add_executable(dxinsp_triangle WIN32 main.cpp cube.hlsl wave.hlsl ${CSO_OUTPUTS})
# Visual Studio would otherwise try to compile the .hlsl files with fxc itself.
set_source_files_properties(cube.hlsl wave.hlsl PROPERTIES VS_TOOL_OVERRIDE "None")
target_link_libraries(dxinsp_triangle PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(dxinsp_triangle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# Debug information in every configuration, so the inspector's stack traces show functions and
# lines of the test application.
if(MSVC)
    target_compile_options(dxinsp_triangle PRIVATE /Zi)
    target_link_options(dxinsp_triangle PRIVATE /DEBUG)
endif()
set_target_properties(dxinsp_triangle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")

# Copy compiled shaders next to the executable.
add_custom_command(TARGET dxinsp_triangle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CSO_OUTPUTS} "$<TARGET_FILE_DIR:dxinsp_triangle>"
    VERBATIM)
