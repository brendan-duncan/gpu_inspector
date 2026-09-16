# A Direct3D 12 path tracer (Ray Tracing in One Weekend's final scene), used to exercise the D3D12
# capture library's ray tracing: procedural acceleration structures, a raytracing state object
# built from a DXIL library, shader tables and DispatchRays. See main.cpp. Configures both as a
# subdirectory of the root project and on its own (cmake -S test/path_tracer/d3d12 -B <dir>).
cmake_minimum_required(VERSION 3.20)
project(dxinsp_path_tracer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "dxinsp_path_tracer is a Direct3D 12 application and builds on Windows only")
endif()

# The library is compiled at build time with dxc, from the Vulkan SDK or a Windows SDK that ships
# it (10.0.20348 and newer keep one in bin/<version>/x64).
file(GLOB WINSDK_BIN_DIRS "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT WINSDK_BIN_DIRS ORDER DESCENDING)
find_program(DXC dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" ${WINSDK_BIN_DIRS})
if(NOT DXC)
    message(FATAL_ERROR "dxc not found (install the Vulkan SDK or set VULKAN_SDK; a recent Windows SDK also has it)")
endif()

set(CSO "${CMAKE_CURRENT_BINARY_DIR}/path_tracer.cso")
add_custom_command(
    OUTPUT "${CSO}"
    # One DXIL library with every entry point. -Zi keeps the HLSL source and line information in
    # the container (the inspector's Source view and line links); -Qembed_debug only silences
    # dxc's warning about having no -Fd to write a PDB to instead.
    COMMAND ${DXC} -T lib_6_3 -Zi -Qembed_debug -Fo "${CSO}" "${CMAKE_CURRENT_SOURCE_DIR}/path_tracer.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/path_tracer.hlsl"
    COMMENT "Compiling path_tracer.hlsl (lib_6_3)"
    VERBATIM)

add_executable(dxinsp_path_tracer WIN32 main.cpp ../scene.h path_tracer.hlsl "${CSO}")
# Visual Studio would otherwise try to compile the .hlsl file with fxc itself.
set_source_files_properties(path_tracer.hlsl PROPERTIES VS_TOOL_OVERRIDE "None")
target_link_libraries(dxinsp_path_tracer PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(dxinsp_path_tracer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# Debug information in every configuration, so the inspector's stack traces show functions and
# lines of the test application.
if(MSVC)
    target_compile_options(dxinsp_path_tracer PRIVATE /Zi)
    target_link_options(dxinsp_path_tracer PRIVATE /DEBUG)
endif()
set_target_properties(dxinsp_path_tracer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Copy the compiled library next to the executable.
add_custom_command(TARGET dxinsp_path_tracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CSO}" "$<TARGET_FILE_DIR:dxinsp_path_tracer>"
    VERBATIM)
