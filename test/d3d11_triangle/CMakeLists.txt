# A small Direct3D 11 application: the counterpart of test/d3d12_triangle for the Direct3D 11
# plugin (src/plugins/d3d11), used to exercise its object tracking and frame capture without a
# real game. Configures both as a subdirectory of the root project and on its own
# (cmake -S test/d3d11_triangle -B <dir>). The shaders are HLSL strings in main.cpp, compiled
# at run time with d3dcompiler, so there is nothing to build but the executable.
cmake_minimum_required(VERSION 3.20)
project(d3d11insp_triangle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "d3d11insp_triangle is a Direct3D 11 application and builds on Windows only")
endif()

add_executable(d3d11insp_triangle WIN32 main.cpp)
# include/gpu_inspector.h, for --capture-at.
target_include_directories(d3d11insp_triangle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(d3d11insp_triangle PRIVATE d3d11 dxgi d3dcompiler dxguid)
target_compile_definitions(d3d11insp_triangle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# Debug information in every configuration, so the inspector's stack traces show functions and
# lines of the test application.
if(MSVC)
    target_compile_options(d3d11insp_triangle PRIVATE /Zi)
    target_link_options(d3d11insp_triangle PRIVATE /DEBUG)
endif()
set_target_properties(d3d11insp_triangle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
