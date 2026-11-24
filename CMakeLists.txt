cmake_minimum_required(VERSION 3.18)
project(seedlessds_libretro C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64"
   AND NOT CMAKE_C_COMPILER_TARGET MATCHES "aarch64")
    if(SEEDLESS_ALLOW_NON_AARCH64)
        message(WARNING
            "Non-AArch64 build: GPU NEON / JIT will not work. "
            "Stubs are for link experiments only.")
    else()
        message(FATAL_ERROR
            "SeedlessDS-Core requires AArch64 (JIT + NEON).\n"
            "Cross-compile example:\n"
            "  make CROSS=aarch64-linux-gnu-\n"
            "Or Android NDK arm64-v8a. "
            "Override with -DSEEDLESS_ALLOW_NON_AARCH64=ON.")
    endif()
endif()

set(SEEDLESS_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core" CACHE PATH
    "Path to SeedlessDS-Core")
set(SEEDLESS_LIBRETRO ON CACHE BOOL "Build libretro frontend" FORCE)
set(SEEDLESS_SKIP_ANDROID_PLATFORM ON CACHE BOOL "Skip Android JNI platform" FORCE)

if(NOT EXISTS "${SEEDLESS_CORE_DIR}/include/seedlessds/nds.h")
    message(FATAL_ERROR "SeedlessDS-Core not found at ${SEEDLESS_CORE_DIR}. "
            "Run: git submodule update --init --recursive")
endif()

find_package(Threads REQUIRED)

# Cross toolchains often lack a target libz; prefer a bundled static zlib then.
set(SEEDLESS_USE_BUNDLED_ZLIB OFF)
if(CMAKE_CROSSCOMPILING OR SEEDLESS_FORCE_BUNDLED_ZLIB)
    set(SEEDLESS_USE_BUNDLED_ZLIB ON)
else()
    find_package(ZLIB QUIET)
    if(NOT ZLIB_FOUND)
        set(SEEDLESS_USE_BUNDLED_ZLIB ON)
    endif()
endif()

if(SEEDLESS_USE_BUNDLED_ZLIB)
    message(STATUS "Building bundled zlib 1.3.1")
    include(FetchContent)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    )
    FetchContent_MakeAvailable(zlib)
    if(TARGET zlibstatic)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
        target_include_directories(zlibstatic PUBLIC
            $<BUILD_INTERFACE:${zlib_SOURCE_DIR}>
            $<BUILD_INTERFACE:${zlib_BINARY_DIR}>)
        set(SEEDLESS_ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    elseif(TARGET zlib)
        add_library(ZLIB::ZLIB ALIAS zlib)
        set(SEEDLESS_ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    else()
        message(FATAL_ERROR "Bundled zlib did not create an expected target")
    endif()
endif()

add_subdirectory("${SEEDLESS_CORE_DIR}/third_party" third_party)

# Core sources land on this OBJECT target (name expected by core CMakeLists).
add_library(recon_fn OBJECT)
target_include_directories(recon_fn PRIVATE
    "${SEEDLESS_CORE_DIR}/include"
    "${SEEDLESS_CORE_DIR}/src"
    "${SEEDLESS_CORE_DIR}/third_party/lua"
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${SEEDLESS_ZLIB_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS})
target_compile_definitions(recon_fn PRIVATE _GNU_SOURCE SEEDLESS_LIBRETRO=1)
target_compile_options(recon_fn PRIVATE
    -O2 -fno-strict-aliasing -fPIC
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable)

include("${SEEDLESS_CORE_DIR}/src/CMakeLists.txt")

# Keep Android audio mixer feed path; replace OpenSL with libretro/opensl_stub.c.
target_sources(recon_fn PRIVATE
    "${SEEDLESS_CORE_DIR}/src/platform/android/audio.c")

# Propagate third_party / zlib include dirs onto OBJECT sources.
target_link_libraries(recon_fn PUBLIC seedless_lua seedless_unrar seedless_lzma ZLIB::ZLIB)

option(SEEDLESS_JIT_PROFILE "Measure JIT block analysis in ROM replay benchmarks" OFF)
if(SEEDLESS_JIT_PROFILE)
    target_compile_definitions(recon_fn PRIVATE SEEDLESS_JIT_PROFILE=1)
endif()

option(SEEDLESS_COMPUTE_PROFILE "Measure GLES upload, dispatch, readback, and copy time" OFF)

set(SEEDLESS_LIBRETRO_SOURCES
    libretro/libretro.c
    libretro/opensl_stub.c
    libretro/gl_stubs.c
    libretro/android_compat.c
    "${SEEDLESS_CORE_DIR}/src/platform/posix/files_posix.c"
)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64"
   AND NOT CMAKE_C_COMPILER_TARGET MATCHES "aarch64")
    list(APPEND SEEDLESS_LIBRETRO_SOURCES
        libretro/adpcm_c.c
        libretro/cpu_stub.c)
endif()

add_library(seedlessds_libretro SHARED
    ${SEEDLESS_LIBRETRO_SOURCES}
    $<TARGET_OBJECTS:recon_fn>
)

set_target_properties(seedlessds_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "seedlessds_libretro"
)

target_include_directories(seedlessds_libretro PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/deps"
    "${CMAKE_CURRENT_SOURCE_DIR}/libretro"
    "${SEEDLESS_CORE_DIR}/include"
    "${SEEDLESS_CORE_DIR}/src"
    "${SEEDLESS_CORE_DIR}/third_party/lua"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_compile_definitions(seedlessds_libretro PRIVATE
    _GNU_SOURCE
    SEEDLESS_LIBRETRO=1
)
if(SEEDLESS_COMPUTE_PROFILE)
    target_compile_definitions(seedlessds_libretro PRIVATE SEEDLESS_COMPUTE_PROFILE=1)
endif()

target_compile_options(seedlessds_libretro PRIVATE
    -O2
    -fno-strict-aliasing
    -Wall
    -Wno-unused-parameter
)

target_link_libraries(seedlessds_libretro PRIVATE
    seedless_lua
    seedless_unrar
    seedless_lzma
    ZLIB::ZLIB
    Threads::Threads
    m
    dl
)

# Match BananaDS's opt-in LTO build for constrained AArch64 targets. Keep this
# disabled by default because some cross toolchains do not ship compatible
# plugin-aware archivers and linkers.
option(SEEDLESS_ENABLE_IPO "Enable interprocedural optimization for the core" OFF)
if(SEEDLESS_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SEEDLESS_IPO_SUPPORTED
        OUTPUT SEEDLESS_IPO_ERROR
        LANGUAGES C CXX)
    if(NOT SEEDLESS_IPO_SUPPORTED)
        message(FATAL_ERROR "IPO is unavailable: ${SEEDLESS_IPO_ERROR}")
    endif()

    set_property(TARGET recon_fn seedlessds_libretro
        PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    foreach(target IN ITEMS seedless_lua seedless_unrar seedless_lzma zlibstatic zlib)
        if(TARGET ${target})
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endforeach()
endif()

if(NOT APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/link.T")
    target_link_options(seedlessds_libretro PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T
        -Wl,-Bsymbolic)
endif()

# Tune AArch64 builds for the RG DS Plus's Cortex-A55 by default without
# raising the baseline instruction set. The cache option remains overridable.
set(SEEDLESS_ARM_TUNE_DEFAULT "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR
   CMAKE_C_COMPILER_TARGET MATCHES "aarch64")
    set(SEEDLESS_ARM_TUNE_DEFAULT "cortex-a55")
endif()
set(SEEDLESS_ARM_TUNE "${SEEDLESS_ARM_TUNE_DEFAULT}" CACHE STRING
    "AArch64 CPU tuning target (defaults to cortex-a55 on AArch64)")
if(SEEDLESS_ARM_TUNE)
    include(CheckCCompilerFlag)
    if(SEEDLESS_ARM_TUNE STREQUAL "cortex-a55")
        set(SEEDLESS_TUNE_CHECK_VAR SEEDLESS_TUNE_cortex_a55_HYPHEN)
    elseif(SEEDLESS_ARM_TUNE STREQUAL "cortex_a55")
        set(SEEDLESS_TUNE_CHECK_VAR SEEDLESS_TUNE_cortex_a55_UNDERSCORE)
    else()
        string(MAKE_C_IDENTIFIER "${SEEDLESS_ARM_TUNE}" SEEDLESS_TUNE_ID)
        set(SEEDLESS_TUNE_CHECK_VAR "SEEDLESS_TUNE_${SEEDLESS_TUNE_ID}")
    endif()
    check_c_compiler_flag("-mtune=${SEEDLESS_ARM_TUNE}" "${SEEDLESS_TUNE_CHECK_VAR}")
    # Zig's AArch64 CPU names use underscores; GCC/Clang use hyphens.
    if(NOT ${SEEDLESS_TUNE_CHECK_VAR} AND SEEDLESS_ARM_TUNE STREQUAL "cortex-a55")
        check_c_compiler_flag("-mtune=cortex_a55" SEEDLESS_TUNE_cortex_a55_UNDERSCORE)
        if(SEEDLESS_TUNE_cortex_a55_UNDERSCORE)
            set(SEEDLESS_ARM_TUNE "cortex_a55" CACHE STRING
                "AArch64 CPU tuning target (defaults to cortex-a55 on AArch64)" FORCE)
            set(SEEDLESS_TUNE_CHECK_VAR SEEDLESS_TUNE_cortex_a55_UNDERSCORE)
        endif()
    endif()
    if(NOT ${SEEDLESS_TUNE_CHECK_VAR})
        message(FATAL_ERROR "Unsupported ARM tuning: ${SEEDLESS_ARM_TUNE}")
    endif()
    foreach(target IN ITEMS recon_fn seedlessds_libretro)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-mtune=${SEEDLESS_ARM_TUNE}>")
    endforeach()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SEEDLESS_GLES_COMPUTE AUTO CACHE STRING "Build the GLES 3.1 rasterizer (AUTO, ON, OFF)")
    set_property(CACHE SEEDLESS_GLES_COMPUTE PROPERTY STRINGS AUTO ON OFF)
    if(SEEDLESS_GLES_COMPUTE)
        add_subdirectory(libretro/compute)
    endif()
endif()

install(TARGETS seedlessds_libretro LIBRARY DESTINATION lib/libretro)
install(FILES seedlessds_libretro.info DESTINATION share/libretro/info OPTIONAL)
