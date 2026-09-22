# The OpenGL ES phone test application (see main.cpp): a NativeActivity drawing with an OpenGL ES 3.2
# context of EGL's, built by tools/build_android_gles_triangle.py with the NDK toolchain.
cmake_minimum_required(VERSION 3.20)
project(android_gles_triangle CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(android_gles_triangle SHARED main.cpp "${GLUE_DIR}/android_native_app_glue.c")
target_include_directories(android_gles_triangle PRIVATE "${GLUE_DIR}")
# The glue's entry point must survive the linker (NativeActivity finds ANativeActivity_onCreate).
target_link_options(android_gles_triangle PRIVATE "-Wl,-u,ANativeActivity_onCreate" "-Wl,-z,max-page-size=16384")
target_link_libraries(android_gles_triangle PRIVATE android log EGL GLESv3)
