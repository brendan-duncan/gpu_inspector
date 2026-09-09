# The phone test application (see main.cpp): a NativeActivity with a swapchain, built by
# tools/build_android_triangle.py with the NDK toolchain. Expects SHADERS_DIR, the directory
# holding the generated shaders.inl.
cmake_minimum_required(VERSION 3.20)
project(android_triangle CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(android_triangle SHARED main.cpp "${GLUE_DIR}/android_native_app_glue.c")
target_include_directories(android_triangle PRIVATE "${GLUE_DIR}" "${SHADERS_DIR}")
target_compile_definitions(android_triangle PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
# The glue's entry point must survive the linker (NativeActivity finds ANativeActivity_onCreate).
target_link_options(android_triangle PRIVATE "-Wl,-u,ANativeActivity_onCreate")
target_link_libraries(android_triangle PRIVATE android log vulkan)
