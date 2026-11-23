find_path(SEEDLESS_EGL_INCLUDE EGL/egl.h)
find_path(SEEDLESS_GLES_INCLUDE GLES3/gl31.h)
find_library(SEEDLESS_EGL_LIBRARY EGL)
if(NOT (SEEDLESS_EGL_INCLUDE AND SEEDLESS_GLES_INCLUDE AND SEEDLESS_EGL_LIBRARY))
    if(SEEDLESS_GLES_COMPUTE STREQUAL "AUTO")
        message(STATUS "EGL/GLES development files unavailable; building the software renderer")
        return()
    endif()
    message(FATAL_ERROR "Compute requires target EGL and GLES 3.1 development files. Install them or configure with -DSEEDLESS_GLES_COMPUTE=OFF.")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS raster.comp)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/raster.comp" RASTER_SHADER)
string(REPLACE "\n" "\\n\"\n\"" RASTER_SHADER "${RASTER_SHADER}")
configure_file(shader.h.in raster_shader.h @ONLY)
foreach(target IN ITEMS recon_fn seedlessds_libretro)
    target_compile_definitions(${target} PRIVATE SEEDLESS_GLES_COMPUTE=1)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
target_sources(seedlessds_libretro PRIVATE commands.c context_linux.c dispatch.c)
target_include_directories(seedlessds_libretro PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${SEEDLESS_EGL_INCLUDE}" "${SEEDLESS_GLES_INCLUDE}")
target_link_libraries(seedlessds_libretro PRIVATE "${SEEDLESS_EGL_LIBRARY}")

# Explicit target for the board. It uses the same sources, definitions and
# archive dependencies as the core, without the libretro export map.
add_executable(seedless_compute_parity EXCLUDE_FROM_ALL
    tests/parity.c commands.c context_linux.c dispatch.c
    $<TARGET_OBJECTS:recon_fn>)
foreach(source IN LISTS SEEDLESS_LIBRETRO_SOURCES)
    if(IS_ABSOLUTE "${source}")
        target_sources(seedless_compute_parity PRIVATE "${source}")
    else()
        target_sources(seedless_compute_parity PRIVATE "${PROJECT_SOURCE_DIR}/${source}")
    endif()
endforeach()
target_include_directories(seedless_compute_parity PRIVATE
    $<TARGET_PROPERTY:seedlessds_libretro,INCLUDE_DIRECTORIES>)
target_compile_definitions(seedless_compute_parity PRIVATE _GNU_SOURCE SEEDLESS_LIBRETRO=1 SEEDLESS_GLES_COMPUTE=1)
target_compile_options(seedless_compute_parity PRIVATE -fno-strict-aliasing)
target_link_libraries(seedless_compute_parity PRIVATE
    $<TARGET_PROPERTY:seedlessds_libretro,LINK_LIBRARIES>)
