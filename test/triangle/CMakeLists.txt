# A small Vulkan application that re-records its command buffer every frame. Used to exercise
# the layer's frame capture (command recording, render target readback) without a Unity build.
cmake_minimum_required(VERSION 3.20)
project(vkinsp_triangle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found (install the Vulkan SDK or set VULKAN_SDK)")
endif()

set(OUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(SHADERS cube.vert cube.frag wave.comp)
set(SPV_FILES)
foreach(s ${SHADERS})
    set(spv "$<TARGET_FILE_DIR:vkinsp_triangle>/${s}.spv")
    list(APPEND SPV_FILES "${spv}")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${s}.spv"
        # -g embeds the GLSL source and line information (the inspector's shader source maps).
        COMMAND ${GLSLC} -g "${CMAKE_CURRENT_SOURCE_DIR}/${s}" -o "${CMAKE_CURRENT_BINARY_DIR}/${s}.spv"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${s}"
        COMMENT "Compiling ${s}"
        VERBATIM)
    list(APPEND SPV_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/${s}.spv")
endforeach()

add_executable(vkinsp_triangle WIN32 main.cpp ${SPV_OUTPUTS})
target_link_libraries(vkinsp_triangle PRIVATE Vulkan::Vulkan)
if(WIN32)
    target_link_libraries(vkinsp_triangle PRIVATE winmm)   # timeBeginPeriod for --offscreen pacing
    target_compile_definitions(vkinsp_triangle PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # Debug information in every configuration, so the inspector's stack traces show functions
    # and lines of the test application.
    if(MSVC)
        target_compile_options(vkinsp_triangle PRIVATE /Zi)
        target_link_options(vkinsp_triangle PRIVATE /DEBUG)
    endif()
else()
    target_compile_definitions(vkinsp_triangle PRIVATE VK_USE_PLATFORM_XCB_KHR)
    target_link_libraries(vkinsp_triangle PRIVATE xcb)
    # Exported symbols, so dladdr names the test application's functions in stack traces.
    target_link_options(vkinsp_triangle PRIVATE -rdynamic)
endif()
set_target_properties(vkinsp_triangle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")

# Copy compiled shaders next to the executable.
add_custom_command(TARGET vkinsp_triangle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPV_OUTPUTS} "$<TARGET_FILE_DIR:vkinsp_triangle>"
    VERBATIM)
