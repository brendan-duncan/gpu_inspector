# The OpenXR test application for Android headsets (see main.cpp); built by
# tools/build_xr_triangle.py with the NDK toolchain. Expects:
#   OPENXR_INCLUDE  the OpenXR headers (openxr/openxr.h)
#   OPENXR_LOADER   libopenxr_loader.so for the ABI
#   SHADERS_DIR     the directory holding the generated shaders.inl
cmake_minimum_required(VERSION 3.20)
project(xr_triangle CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(xr_triangle SHARED main.cpp "${GLUE_DIR}/android_native_app_glue.c")
target_include_directories(xr_triangle PRIVATE "${GLUE_DIR}" "${OPENXR_INCLUDE}" "${SHADERS_DIR}")
target_compile_definitions(xr_triangle PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
# The glue's entry point must survive the linker (NativeActivity finds ANativeActivity_onCreate).
target_link_options(xr_triangle PRIVATE "-Wl,-u,ANativeActivity_onCreate")
target_link_libraries(xr_triangle PRIVATE "${OPENXR_LOADER}" android log vulkan)
