# A Metal path tracer (Ray Tracing in One Weekend's final scene), the counterpart of
# test/path_tracer/vulkan and test/path_tracer/d3d12: acceleration structure encoders, a bounding
# box intersection function in an intersection function table, and a compute kernel that traces.
# See main.mm.
cmake_minimum_required(VERSION 3.20)
project(mtlinsp_path_tracer OBJCXX)

set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

add_executable(mtlinsp_path_tracer main.mm ../scene.h)
target_compile_options(mtlinsp_path_tracer PRIVATE -fobjc-arc -Wall -Wno-unused-parameter -g)
target_link_libraries(mtlinsp_path_tracer PRIVATE
    "-framework Cocoa"
    "-framework Metal"
    "-framework QuartzCore")

# A plain executable, so it is ad-hoc signed without the hardened runtime and
# DYLD_INSERT_LIBRARIES reaches it; see src/metal/README.md.
set_target_properties(mtlinsp_path_tracer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
