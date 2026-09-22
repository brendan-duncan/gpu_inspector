# The Direct3D 11 plugin's capture library (README.md, docs/PLUGINS.md).
#
#   d3d11insp_capture.dll  injected by the inspector's launcher beside the D3D12 library; hooks
#                          d3d11.dll's entry points and the objects they return, and speaks the
#                          inspector's protocol through the plugin SDK (src/sdk).
#
# It is built into build/plugins/d3d11/bin, beside the plugin.json and the backend module the app's
# build writes there (src/app/build.mjs), which is the directory the app loads the plugin from.
cmake_minimum_required(VERSION 3.20)
project(GpuInspectorD3D11Plugin CXX C)

if(NOT WIN32)
    message(STATUS "Direct3D 11 plugin: Windows only, not built")
    return()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(D3D11INSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(D3D11INSP_OUT "${CMAKE_BINARY_DIR}/plugins/d3d11")

# The enum tables and vtable slots are generated from the Windows SDK headers (tools/gen_d3d11.py)
# and committed, so a build needs neither Python nor a particular SDK; they are regenerated when
# the generator changes.
find_package(Python3 COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")
set(GEN_OUTPUTS "${GEN_DIR}/d3d11_enums.gen.h" "${GEN_DIR}/d3d11_enums.gen.cpp" "${GEN_DIR}/d3d11_vtables.gen.h")
if(Python3_FOUND)
    add_custom_command(
        OUTPUT ${GEN_OUTPUTS}
        COMMAND ${Python3_EXECUTABLE} "${D3D11INSP_ROOT}/tools/gen_d3d11.py" --out "${GEN_DIR}"
        DEPENDS "${D3D11INSP_ROOT}/tools/gen_d3d11.py"
        COMMENT "Generating the D3D11 enum and vtable tables from the Windows SDK"
        VERBATIM)
endif()

if(NOT TARGET minhook)
    set(MINHOOK_DIR "${D3D11INSP_ROOT}/third_party/minhook")
    if(NOT EXISTS "${MINHOOK_DIR}/src/hook.c")
        message(FATAL_ERROR "third_party/minhook is empty: run `git submodule update --init`")
    endif()
    add_library(minhook STATIC
        "${MINHOOK_DIR}/src/buffer.c"
        "${MINHOOK_DIR}/src/hook.c"
        "${MINHOOK_DIR}/src/trampoline.c"
        "${MINHOOK_DIR}/src/hde/hde64.c")
    target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")
endif()

add_library(d3d11insp_capture SHARED
    ${GEN_OUTPUTS}
    src/common.h
    src/hook.h
    src/hook.cpp
    src/hooks.h
    src/hooks_object.cpp
    src/hooks_device.cpp
    src/hooks_context.cpp
    src/hooks_dxgi.cpp
    src/state.h
    src/state.cpp
    src/formats.h
    src/formats.cpp
    src/serialize.h
    src/serialize.cpp
    src/shader_reflect.h
    src/shader_reflect.cpp
    src/capture.h
    src/capture.cpp
    src/server.h
    src/server.cpp
    src/platform_win32.cpp)
target_include_directories(d3d11insp_capture PRIVATE "${D3D11INSP_ROOT}/src/sdk/include")
target_link_libraries(d3d11insp_capture PRIVATE minhook dxguid ws2_32 iphlpapi)
target_compile_definitions(d3d11insp_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(d3d11insp_capture PRIVATE /W4 /wd4100 /wd4505 /bigobj)
endif()

set_target_properties(d3d11insp_capture PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${D3D11INSP_OUT}/bin" LIBRARY_OUTPUT_DIRECTORY "${D3D11INSP_OUT}/bin")
foreach(config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${config}" CONFIG)
    set_target_properties(d3d11insp_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CONFIG} "${D3D11INSP_OUT}/bin" LIBRARY_OUTPUT_DIRECTORY_${CONFIG} "${D3D11INSP_OUT}/bin")
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${D3D11INSP_OUT}/plugin.json" COPYONLY)
