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
option(VKINSP_BUILD_REPLAY "Build vkinsp_replay, the capture replay tool" ON)
option(VKINSP_BUILD_PLUGINS "Build the plugins in src/plugins (docs/PLUGINS.md)" ON)

# The Vulkan layer does not build for Apple targets; macOS builds the Metal capture library and
# its test application instead. See src/metal/README.md and the macOS section of README.md.
if(APPLE)
    add_subdirectory(src/metal)
    if(VKINSP_BUILD_TESTS)
        add_subdirectory(test/metal_triangle)
        add_subdirectory(test/path_tracer/metal)
    endif()
else()
    add_subdirectory(src/vulkan)
    if(VKINSP_BUILD_REPLAY AND NOT ANDROID)
        add_subdirectory(src/replay)
    endif()
    if(VKINSP_BUILD_TESTS AND NOT ANDROID)
        add_subdirectory(test/triangle)
        add_subdirectory(test/path_tracer/vulkan)
    endif()
    # Windows also builds the Direct3D 12 capture library, its launcher and its shader tool
    # (src/d3d12/README.md); the app injects the library into every local target beside the Vulkan
    # layer, and whichever API the application uses connects.
    if(WIN32)
        add_subdirectory(src/d3d12)
        if(VKINSP_BUILD_TESTS)
            add_subdirectory(test/d3d12_triangle)
            add_subdirectory(test/path_tracer/d3d12)
        endif()
        # Plugins: APIs the app loads at run time rather than builds in (docs/PLUGINS.md). The OpenGL ES
        # one is the example the plugin SDK (src/sdk) was written against; on Windows it captures ANGLE.
        if(VKINSP_BUILD_PLUGINS)
            add_subdirectory(src/plugins/gles)
            if(VKINSP_BUILD_TESTS)
                add_subdirectory(test/gles_triangle)
            endif()
        endif()
    endif()
endif()
