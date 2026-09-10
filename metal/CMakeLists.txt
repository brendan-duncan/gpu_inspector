# The Metal capture library: a dylib loaded into the target with DYLD_INSERT_LIBRARIES, which
# intercepts Metal by interposing its device entry points and hooking the Objective-C classes it
# reaches from them. It speaks the inspector's protocol — the Vulkan layer's, byte for byte — so
# the Inspect and Capture panels work against a Metal application. See src/interpose.mm and
# README.md.
cmake_minimum_required(VERSION 3.20)
project(GpuInspectorMetalCapture OBJCXX)

set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CAPTURE_LIB "mtlinsp_capture")

add_library(${CAPTURE_LIB} SHARED
    src/swizzle.h
    src/swizzle.mm
    src/hooks.h
    src/hooks_common.h
    src/hooks_descriptors.mm
    src/hooks_device.mm
    src/hooks_command_buffer.mm
    src/hooks_encoders.mm
    src/capture.h
    src/capture.mm
    src/formats.h
    src/formats.mm
    src/frame_stats.h
    src/frame_stats.mm
    src/gpu_trace.h
    src/gpu_trace.mm
    src/image.h
    src/image.mm
    src/reflection.h
    src/reflection.mm
    src/tracker.h
    src/tracker.mm
    src/ui_messages.h
    src/ui_messages.mm
    src/validation.h
    src/validation.mm
    src/transport.h
    src/transport.mm
    src/interpose.mm)

# ../layer/src for json_writer.h, which is header-only, has no Vulkan in it, and writes the same
# JSON the UI already reads. Shared rather than copied so the two backends cannot drift apart.
target_include_directories(${CAPTURE_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../layer/src")

# Manual reference counting, not ARC: the hooks forward to the implementations they replaced
# through raw IMPs, and the `new*` methods hand back an object the caller owns. Passing that
# straight through is only clearly correct with no compiler-inserted retains and releases in the
# way. -fno-objc-arc is the default, but the intent is worth pinning down.
target_compile_options(${CAPTURE_LIB} PRIVATE -fno-objc-arc -Wall -Wno-unused-parameter -g)

# CoreGraphics for CGDirectDisplayCopyCurrentMetalDevice, one of the interposed entry points;
# QuartzCore for CAMetalLayer.
target_link_libraries(${CAPTURE_LIB} PRIVATE
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework CoreGraphics")

set_target_properties(${CAPTURE_LIB} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
