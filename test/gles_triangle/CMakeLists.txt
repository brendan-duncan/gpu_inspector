# A small OpenGL ES 3.0 application on ANGLE, for the OpenGL ES plugin (src/plugins/gles). Configures
# both as a subdirectory of the root project and on its own (cmake -S test/gles_triangle -B <dir>).
#
# ANGLE is not part of Windows, and nothing here builds it: libEGL.dll and libGLESv2.dll are copied
# from ANGLE_DIR beside the executable. Any application that ships ANGLE has them -- Firefox, an
# Electron application's node_modules/electron/dist -- and the first of the places below that has
# both is used when ANGLE_DIR is not set.
cmake_minimum_required(VERSION 3.20)
project(glesinsp_triangle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "glesinsp_triangle loads ANGLE's Windows libraries and builds on Windows only")
endif()

set(ANGLE_DIR "$ENV{ANGLE_DIR}" CACHE PATH "Directory holding ANGLE's libEGL.dll and libGLESv2.dll")
if(NOT ANGLE_DIR)
    foreach(candidate "C:/Program Files/Mozilla Firefox" "C:/Program Files/Firefox Nightly")
        if(EXISTS "${candidate}/libEGL.dll" AND EXISTS "${candidate}/libGLESv2.dll")
            set(ANGLE_DIR "${candidate}" CACHE PATH "Directory holding ANGLE's libEGL.dll and libGLESv2.dll" FORCE)
            break()
        endif()
    endforeach()
endif()

add_executable(glesinsp_triangle main.cpp)
target_compile_definitions(glesinsp_triangle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(glesinsp_triangle PRIVATE /Zi)
    target_link_options(glesinsp_triangle PRIVATE /DEBUG)
endif()
set_target_properties(glesinsp_triangle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(ANGLE_DIR AND EXISTS "${ANGLE_DIR}/libEGL.dll" AND EXISTS "${ANGLE_DIR}/libGLESv2.dll")
    message(STATUS "glesinsp_triangle: ANGLE from ${ANGLE_DIR}")
    add_custom_command(TARGET glesinsp_triangle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ANGLE_DIR}/libEGL.dll" "${ANGLE_DIR}/libGLESv2.dll"
                "$<TARGET_FILE_DIR:glesinsp_triangle>"
        VERBATIM)
    # ANGLE's D3D11 backend compiles shaders with d3dcompiler_47.dll, which Windows has in System32 but
    # an ANGLE build may prefer its own copy of.
    if(EXISTS "${ANGLE_DIR}/d3dcompiler_47.dll")
        add_custom_command(TARGET glesinsp_triangle POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ANGLE_DIR}/d3dcompiler_47.dll" "$<TARGET_FILE_DIR:glesinsp_triangle>"
            VERBATIM)
    endif()
else()
    message(STATUS "glesinsp_triangle: no ANGLE found (set ANGLE_DIR); it runs with --angle <dir> instead")
endif()
