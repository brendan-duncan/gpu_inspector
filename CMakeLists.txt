cmake_minimum_required(VERSION 3.20)
project(GpuInspector CXX)

option(VKINSP_BUILD_TESTS "Build the test applications" ON)

add_subdirectory(layer)

if(VKINSP_BUILD_TESTS)
    add_subdirectory(test/triangle)
endif()
