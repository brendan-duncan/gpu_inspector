# The OpenGL ES test application for Linux (see main.cpp). Needs the EGL and OpenGL ES headers and
# libraries (libegl-dev and libgles-dev on Debian and Ubuntu, mesa-libEGL-devel on Fedora); without
# them it is left out.
find_path(GLES_LINUX_EGL_INCLUDE EGL/egl.h)
find_path(GLES_LINUX_GLES_INCLUDE GLES3/gl32.h)
find_library(GLES_LINUX_EGL EGL)
find_library(GLES_LINUX_GLESV2 GLESv2)
if(NOT GLES_LINUX_EGL_INCLUDE OR NOT GLES_LINUX_GLES_INCLUDE OR NOT GLES_LINUX_EGL OR NOT GLES_LINUX_GLESV2)
    message(STATUS "gles_linux: the EGL and OpenGL ES development files are not installed (libegl-dev, libgles-dev), not built")
    return()
endif()

add_executable(gles_linux main.cpp)
target_compile_features(gles_linux PRIVATE cxx_std_17)
target_include_directories(gles_linux PRIVATE "${GLES_LINUX_EGL_INCLUDE}" "${GLES_LINUX_GLES_INCLUDE}")
target_link_libraries(gles_linux PRIVATE "${GLES_LINUX_EGL}" "${GLES_LINUX_GLESV2}" ${CMAKE_DL_LIBS})
set_target_properties(gles_linux PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# SDL (libsdl2-dev) gives the application a real window, and reaches EGL the way an engine does: it
# dlopens libEGL.so.1 itself, so a windowed run exercises the plugin's dlopen redirect and
# eglCreateWindowSurface, which the pbuffer path never does. Optional: without it the application is
# built offscreen-only, and says so.
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    # The config file is inconsistent across distributions: a target on some, variables on others.
    if(TARGET SDL2::SDL2)
        target_link_libraries(gles_linux PRIVATE SDL2::SDL2)
    else()
        target_include_directories(gles_linux PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(gles_linux PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_compile_definitions(gles_linux PRIVATE GLES_LINUX_HAVE_SDL=1)
    message(STATUS "gles_linux: with SDL2, so it can open a window")
else()
    message(STATUS "gles_linux: SDL2 not found (libsdl2-dev), the windowed mode is left out")
endif()
