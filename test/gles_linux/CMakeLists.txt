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
