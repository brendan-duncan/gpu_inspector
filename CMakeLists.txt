cmake_minimum_required(VERSION 3.20)

# The oldest macOS the Metal capture library runs on. Set before project(), because the compiler
# checks bake it in, and unconditionally, because APPLE is not defined until project() has run
# and every other platform ignores this. Without it the library takes the build machine's macOS
# version as its minimum, so one built on a current runner will not load on anything older —
# silently, from the application's point of view. 11.0 is what the sources already assume:
# everything newer than that sits behind an @available check.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version the build runs on")
endif()

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
