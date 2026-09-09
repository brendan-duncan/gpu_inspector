cmake_minimum_required(VERSION 3.20)
project(GpuInspector CXX)

option(VKINSP_BUILD_TESTS "Build the test applications" ON)

# The Vulkan layer does not build for Apple targets; macOS builds the Metal capture library and
# its test application instead. See metal/README.md and the macOS section of README.md.
if(APPLE)
    add_subdirectory(metal)
    if(VKINSP_BUILD_TESTS)
        add_subdirectory(test/metal_triangle)
    endif()
else()
    add_subdirectory(layer)
    if(VKINSP_BUILD_TESTS AND NOT ANDROID)
        add_subdirectory(test/triangle)
    endif()
endif()
