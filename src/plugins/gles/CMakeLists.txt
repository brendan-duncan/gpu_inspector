# The OpenGL ES plugin's capture library (README.md, docs/PLUGINS.md).
#
#   glesinsp_capture.dll  injected by the inspector's launcher beside the D3D12 library; hooks ANGLE's
#                         libGLESv2.dll and libEGL.dll and speaks the inspector's protocol through the
#                         plugin SDK (src/sdk).
#
# It is built into build/plugins/gles/bin, beside the plugin.json and the backend module the app's build
# writes there (src/app/build.mjs), which is the directory the app loads the plugin from.
cmake_minimum_required(VERSION 3.20)
project(GpuInspectorGlesPlugin CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLESINSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(GLESINSP_OUT "${CMAKE_BINARY_DIR}/plugins/gles")

# The entry points are generated from the Khronos registry (tools/gen_gles.py) and committed, so a build
# needs neither Python nor the registry; they are regenerated when the generator changes.
find_package(Python3 COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")
set(GEN_OUTPUTS "${GEN_DIR}/gles_api.gen.h" "${GEN_DIR}/gles_hooks.gen.cpp" "${GEN_DIR}/gles_enums.gen.h"
                "${GEN_DIR}/gles_enums.gen.cpp" "${GEN_DIR}/gles_constants.gen.h")
if(Python3_FOUND)
    add_custom_command(
        OUTPUT ${GEN_OUTPUTS}
        COMMAND ${Python3_EXECUTABLE} "${GLESINSP_ROOT}/tools/gen_gles.py" --out "${GEN_DIR}"
        DEPENDS "${GLESINSP_ROOT}/tools/gen_gles.py"
        COMMENT "Generating the OpenGL ES entry points from gl.xml"
        VERBATIM)
endif()

# Linux: the library stands in for the system's libEGL and libGLESv2 (platform_linux.cpp), so it links
# them, and a dlsym() from its handle finds in them what it does not hook. Without them there is no
# OpenGL ES to capture, and the plugin is left out.
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_library(GLESINSP_LIBEGL NAMES libEGL.so.1 EGL)
    find_library(GLESINSP_LIBGLESV2 NAMES libGLESv2.so.2 GLESv2)
    if(NOT GLESINSP_LIBEGL OR NOT GLESINSP_LIBGLESV2)
        message(STATUS "OpenGL ES plugin: libEGL.so.1 or libGLESv2.so.2 not found (install libegl1 and libgles2), not built")
        return()
    endif()
    find_package(Threads REQUIRED)
endif()

if(WIN32 AND NOT TARGET minhook)
    set(MINHOOK_DIR "${GLESINSP_ROOT}/third_party/minhook")
    add_library(minhook STATIC
        "${MINHOOK_DIR}/src/buffer.c"
        "${MINHOOK_DIR}/src/hook.c"
        "${MINHOOK_DIR}/src/trampoline.c"
        "${MINHOOK_DIR}/src/hde/hde64.c")
    target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")
endif()

add_library(glesinsp_capture SHARED
    ${GEN_OUTPUTS}
    src/runtime.h
    src/egl_api.h
    src/state.h
    src/state.cpp
    src/formats.h
    src/formats.cpp
    src/capture.h
    src/capture.cpp
    src/hooks_gl.cpp
    src/hooks_egl.cpp
    src/server.h
    src/server.cpp)
if(WIN32)
    target_sources(glesinsp_capture PRIVATE src/platform_win32.h src/platform_win32.cpp src/hooks_wgl.cpp)
    target_link_libraries(glesinsp_capture PRIVATE minhook)
    target_compile_definitions(glesinsp_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
elseif(ANDROID)
    # An OpenGL ES layer (platform_android.cpp), which tools/build_android.py copies to
    # build/plugins/gles/android/lib/<abi>.
    target_sources(glesinsp_capture PRIVATE src/platform_android.cpp)
    target_link_libraries(glesinsp_capture PRIVATE log)
    # 16 KB pages: Android 15+ devices may use them, and a library aligned for 4 KB does not load there.
    target_link_options(glesinsp_capture PRIVATE "-Wl,--no-undefined" "-Wl,-z,max-page-size=16384")
elseif(UNIX AND NOT APPLE)
    # Preloaded (LD_PRELOAD): platform_linux.cpp, and GLX's OpenGL ES contexts (hooks_glx.cpp). Only the
    # entry points it stands in for are exported; --no-as-needed keeps the libraries it links, which it
    # calls only through dlsym, as its dependencies.
    target_sources(glesinsp_capture PRIVATE src/platform_linux.cpp src/hooks_glx.cpp)
    set_target_properties(glesinsp_capture PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_link_libraries(glesinsp_capture PRIVATE "-Wl,--no-as-needed" "${GLESINSP_LIBEGL}" "${GLESINSP_LIBGLESV2}" "-Wl,--as-needed"
                          ${CMAKE_DL_LIBS} Threads::Threads)
    target_link_options(glesinsp_capture PRIVATE "-Wl,--no-undefined")
    target_compile_options(glesinsp_capture PRIVATE -Wall -Wno-unused-parameter)
endif()
target_include_directories(glesinsp_capture PRIVATE "${GLESINSP_ROOT}/src/sdk/include")
if(MSVC)
    target_compile_options(glesinsp_capture PRIVATE /W4 /wd4100 /wd4505 /bigobj)
endif()

set_target_properties(glesinsp_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${GLESINSP_OUT}/bin" LIBRARY_OUTPUT_DIRECTORY "${GLESINSP_OUT}/bin")
foreach(config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${config}" CONFIG)
    set_target_properties(glesinsp_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CONFIG} "${GLESINSP_OUT}/bin" LIBRARY_OUTPUT_DIRECTORY_${CONFIG} "${GLESINSP_OUT}/bin")
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${GLESINSP_OUT}/plugin.json" COPYONLY)
