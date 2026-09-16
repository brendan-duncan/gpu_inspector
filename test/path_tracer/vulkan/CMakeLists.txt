# A Vulkan path tracer (Ray Tracing in One Weekend's final scene), used to exercise the layer's
# ray tracing capture: bounding-box acceleration structures, a pipeline with intersection and
# per-material closest hit shaders, a shader binding table with instance offsets, and a storage
# image carried from frame to frame. See main.cpp.
cmake_minimum_required(VERSION 3.20)
project(vkinsp_path_tracer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found (install the Vulkan SDK or set VULKAN_SDK)")
endif()

set(SHADERS path.rgen sky.rmiss sphere.rint lambertian.rchit metal.rchit dielectric.rchit)
set(SPV_OUTPUTS)
foreach(s ${SHADERS})
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${s}.spv"
        # -g embeds the GLSL source and line information (the inspector's shader source maps);
        # ray tracing stages need SPIR-V 1.4.
        COMMAND ${GLSLC} -g --target-env=vulkan1.2 "${CMAKE_CURRENT_SOURCE_DIR}/${s}" -o "${CMAKE_CURRENT_BINARY_DIR}/${s}.spv"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${s}" "${CMAKE_CURRENT_SOURCE_DIR}/common.glsl"
        COMMENT "Compiling ${s}"
        VERBATIM)
    list(APPEND SPV_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/${s}.spv")
endforeach()

add_executable(vkinsp_path_tracer WIN32 main.cpp ../scene.h ${SPV_OUTPUTS})
target_link_libraries(vkinsp_path_tracer PRIVATE Vulkan::Vulkan)
if(WIN32)
    target_compile_definitions(vkinsp_path_tracer PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # Debug information in every configuration, so the inspector's stack traces show functions
    # and lines of the test application.
    if(MSVC)
        target_compile_options(vkinsp_path_tracer PRIVATE /Zi)
        target_link_options(vkinsp_path_tracer PRIVATE /DEBUG)
    endif()
else()
    target_compile_definitions(vkinsp_path_tracer PRIVATE VK_USE_PLATFORM_XCB_KHR)
    target_link_libraries(vkinsp_path_tracer PRIVATE xcb)
    # Exported symbols, so dladdr names the test application's functions in stack traces.
    target_link_options(vkinsp_path_tracer PRIVATE -rdynamic)
endif()
set_target_properties(vkinsp_path_tracer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Copy compiled shaders next to the executable.
add_custom_command(TARGET vkinsp_path_tracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPV_OUTPUTS} "$<TARGET_FILE_DIR:vkinsp_path_tracer>"
    VERBATIM)
