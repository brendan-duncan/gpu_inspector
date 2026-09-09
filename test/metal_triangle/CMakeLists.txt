# A small Metal application to point the Metal capture library at, the counterpart of
# test/triangle. See main.mm.
cmake_minimum_required(VERSION 3.20)
project(mtlinsp_triangle OBJCXX)

set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

add_executable(mtlinsp_triangle main.mm)
target_compile_options(mtlinsp_triangle PRIVATE -fobjc-arc -Wall -Wno-unused-parameter -g)
target_link_libraries(mtlinsp_triangle PRIVATE
    "-framework Cocoa"
    "-framework Metal"
    "-framework QuartzCore")

# A plain executable, so it is ad-hoc signed without the hardened runtime and
# DYLD_INSERT_LIBRARIES reaches it. A target with the hardened runtime and no
# com.apple.security.cs.allow-dyld-environment-variables entitlement silently drops the variable;
# see metal/README.md.
set_target_properties(mtlinsp_triangle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
