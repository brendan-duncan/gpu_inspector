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
    src/cpu_timeline.h
    src/cpu_timeline.mm
    src/draw_overlay.mm
    src/formats.h
    src/formats.mm
    src/function_constants.mm
    src/frame_stats.h
    src/frame_stats.mm
    src/gpu_trace.h
    src/gpu_trace.mm
    src/hud.h
    src/hud.mm
    src/image.h
    src/image.mm
    src/overdraw.h
    src/overdraw.mm
    src/pass_record.h
    src/pixel_history.mm
    src/raytracing.h
    src/raytracing.mm
    src/reflection.h
    src/reflection.mm

    src/shader_edit.mm
    src/stacktrace.h
    src/stacktrace.mm
    src/tracker.h
    src/tracker.mm
    src/ui_messages.h
    src/ui_messages.mm
    src/validation.h
    src/validation.mm
    src/transport.h
    src/transport.mm
    src/interpose.mm)

# ../vulkan/src for json_writer.h, which is header-only, has no Vulkan in it, and writes the same
# JSON the UI already reads. Shared rather than copied so the two backends cannot drift apart. The
# HUD's hud_text.h and the live pause's frame_pause.h come from there for the same reason: neither
# has any graphics API in it, so the HUD reads the same and the pause behaves the same on all three.
target_include_directories(${CAPTURE_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/src")

# Manual reference counting, not ARC: the hooks forward to the implementations they replaced
# through raw IMPs, and the `new*` methods hand back an object the caller owns. Passing that
# straight through is only clearly correct with no compiler-inserted retains and releases in the
# way. -fno-objc-arc is the default, but the intent is worth pinning down.
target_compile_options(${CAPTURE_LIB} PRIVATE -fno-objc-arc -Wall -Wno-unused-parameter -g)

# CoreGraphics for CGDirectDisplayCopyCurrentMetalDevice, one of the interposed entry points;
# QuartzCore for CAMetalLayer; AppKit for the capture hotkey's NSEvent monitor, which is how the
# HUD sees a key pressed in the application's own window (../vulkan/src/hud_hotkey.h).
target_link_libraries(${CAPTURE_LIB} PRIVATE
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework AppKit")

set_target_properties(${CAPTURE_LIB} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ------------------------------------------------------------------------------------------------
# mtlinsp_replay: re-executes a Metal capture on this machine's GPU and writes it out as a C++
# project (replay/, docs/REPLAY.md "Metal"). The capture file, its JSON and the arena are the Vulkan
# replay's (src/replay/src), which have no graphics API in them; the pixel format tables are the
# capture library's. The exported project's hand-written files live in replay/export_template as
# real sources, embedded into the tool as strings.
set(MTLINSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(MTL_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")

# The generated enum tables come from the Metal SDK headers (tools/gen_metal_enums.py) and are
# committed, so a build needs neither Python nor a particular SDK to compile them.
#
# Regenerating is a deliberate step (`cmake --build . --target mtlinsp_metal_enums`, or running the
# generator by hand) rather than part of the build: the tables are only as complete as the SDK they
# are read from, so a build on a machine with an older SDK would otherwise silently rewrite the
# committed ones without the enums that SDK has never heard of — MTLMatrixLayout is macOS 15 — and
# then fail to compile the replay sources that name them. Regenerate on the newest SDK and commit
# the result.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(mtlinsp_metal_enums
        COMMAND ${Python3_EXECUTABLE} "${MTLINSP_ROOT}/tools/gen_metal_enums.py" --out "${MTL_GEN_DIR}"
        COMMENT "Generating Metal enum tables from the Metal SDK"
        VERBATIM)
endif()

if(VKINSP_BUILD_REPLAY AND Python3_Interpreter_FOUND)
    set(MTLREPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/replay")
    set(VKREPLAY_SRC "${MTLINSP_ROOT}/src/replay/src")
    set(MTLREPLAY_TEMPLATES
        "${MTLREPLAY_DIR}/export_template/main.mm"
        "${MTLREPLAY_DIR}/export_template/mtl_support.h"
        "${MTLREPLAY_DIR}/export_template/mtl_support.mm"
        # The window, shared with the Vulkan export (src/replay/export_template): Metal needs the Cocoa one only.
        "${MTLINSP_ROOT}/src/replay/export_template/frame_window.h"
        "${MTLINSP_ROOT}/src/replay/export_template/frame_window_cocoa.mm")
    set(MTLREPLAY_TEMPLATES_CPP "${CMAKE_CURRENT_BINARY_DIR}/mtl_export_templates.gen.cpp")
    add_custom_command(
        OUTPUT "${MTLREPLAY_TEMPLATES_CPP}"
        COMMAND ${Python3_EXECUTABLE} "${MTLINSP_ROOT}/tools/embed_files.py" --out "${MTLREPLAY_TEMPLATES_CPP}"
                --symbol kMtlExportTemplates --namespace mtlreplay ${MTLREPLAY_TEMPLATES}
        DEPENDS "${MTLINSP_ROOT}/tools/embed_files.py" ${MTLREPLAY_TEMPLATES}
        COMMENT "Embedding the exported Metal project's support files"
        VERBATIM)

    add_executable(mtlinsp_replay
        replay/src/main.mm
        replay/src/mtl_reflect.h
        replay/src/mtl_decode.h
        replay/src/mtl_decode.mm
        replay/src/mtl_source.h
        replay/src/mtl_source.mm
        replay/src/mtl_replayer.h
        replay/src/mtl_replayer.mm
        replay/src/mtl_commands.mm
        replay/src/mtl_raytracing.mm
        replay/src/mtl_exporter.h
        replay/src/mtl_exporter.mm
        src/formats.mm
        "${VKREPLAY_SRC}/json.cpp"
        "${VKREPLAY_SRC}/gpucap.cpp"
        "${VKREPLAY_SRC}/decode.cpp"
        "${MTL_GEN_DIR}/metal_enums.gen.cpp"
        "${MTLREPLAY_TEMPLATES_CPP}")
    set_target_properties(mtlinsp_replay PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    target_include_directories(mtlinsp_replay PRIVATE
        "${MTLREPLAY_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MTL_GEN_DIR}"
        "${VKREPLAY_SRC}"
        "${MTLINSP_ROOT}/src/vulkan/src")
    # ARC, unlike the capture library: nothing here is hooked or forwarded, so letting the compiler
    # own the objects is simply safer, and the replay holds every object for its whole run.
    target_compile_options(mtlinsp_replay PRIVATE -fobjc-arc -Wall -Wno-unused-parameter)
    target_link_libraries(mtlinsp_replay PRIVATE
        "-framework Foundation"
        "-framework Metal")
endif()
