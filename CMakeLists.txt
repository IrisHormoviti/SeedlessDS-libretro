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
    -fno-strict-aliasing -fPIC
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

# Match BananaDS's LTO build on constrained AArch64 targets when the toolchain
# exposes its compiler-aware archivers. Keep generic and incomplete wrappers
# on the normal build path unless IPO is explicitly requested.
set(SEEDLESS_IPO_DEFAULT OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR
   CMAKE_C_COMPILER_TARGET MATCHES "aarch64")
    if(CMAKE_C_COMPILER_AR AND NOT CMAKE_C_COMPILER_AR MATCHES "NOTFOUND" AND
       CMAKE_CXX_COMPILER_AR AND NOT CMAKE_CXX_COMPILER_AR MATCHES "NOTFOUND")
        set(SEEDLESS_IPO_DEFAULT ON)
    endif()
endif()
set(SEEDLESS_IPO_AUTO_DEFAULT OFF)
if(NOT DEFINED SEEDLESS_ENABLE_IPO)
    set(SEEDLESS_IPO_AUTO_DEFAULT ON)
endif()
option(SEEDLESS_ENABLE_IPO "Enable interprocedural optimization for the core"
       ${SEEDLESS_IPO_DEFAULT})
set(SEEDLESS_IPO_ACTIVE OFF)
if(SEEDLESS_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SEEDLESS_IPO_SUPPORTED
        OUTPUT SEEDLESS_IPO_ERROR
        LANGUAGES C CXX)
    if(NOT SEEDLESS_IPO_SUPPORTED)
        if(SEEDLESS_IPO_AUTO_DEFAULT)
            message(WARNING "IPO auto-detection failed; using the normal build: ${SEEDLESS_IPO_ERROR}")
            # The cached option value came from automatic detection, not an
            # explicit user request. Persist the fallback so a second
            # configure of the same build directory remains successful.
            set(SEEDLESS_ENABLE_IPO OFF CACHE BOOL
                "Enable interprocedural optimization for the core" FORCE)
        else()
            message(FATAL_ERROR "IPO is unavailable: ${SEEDLESS_IPO_ERROR}")
        endif()
    else()
        set(SEEDLESS_IPO_ACTIVE ON)
    endif()
endif()

if(SEEDLESS_IPO_ACTIVE)
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

# Target AArch64 builds at the RG DS Plus's Cortex-A55 by default. The native
# CPU option lets GCC use the A55 instruction set as well as its scheduler
# tuning; builders targeting a different ARM64 CPU can override the target.
set(SEEDLESS_ARM_CPU_DEFAULT "")
set(SEEDLESS_ARM_USE_NATIVE_CPU_DEFAULT OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR
   CMAKE_C_COMPILER_TARGET MATCHES "aarch64")
    set(SEEDLESS_ARM_CPU_DEFAULT "cortex-a55")
    set(SEEDLESS_ARM_USE_NATIVE_CPU_DEFAULT ON)
endif()
set(SEEDLESS_ARM_CPU "${SEEDLESS_ARM_CPU_DEFAULT}" CACHE STRING
    "AArch64 CPU target (defaults to cortex-a55 on AArch64; empty disables CPU tuning)")
option(SEEDLESS_ARM_USE_NATIVE_CPU
    "Allow instructions specific to SEEDLESS_ARM_CPU"
    ${SEEDLESS_ARM_USE_NATIVE_CPU_DEFAULT})
if(SEEDLESS_ARM_CPU)
    string(MAKE_C_IDENTIFIER "${SEEDLESS_ARM_CPU}" SEEDLESS_CPU_ID)
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)
    if(SEEDLESS_ARM_USE_NATIVE_CPU)
        set(SEEDLESS_CPU_FLAG "-mcpu=${SEEDLESS_ARM_CPU}")
    else()
        set(SEEDLESS_CPU_FLAG "-mtune=${SEEDLESS_ARM_CPU}")
    endif()
    if(SEEDLESS_ARM_USE_NATIVE_CPU)
        set(SEEDLESS_CPU_MODE native)
    else()
        set(SEEDLESS_CPU_MODE tune)
    endif()
    set(SEEDLESS_CPU_C_CHECK "SEEDLESS_CPU_${SEEDLESS_CPU_ID}_${SEEDLESS_CPU_MODE}_C")
    set(SEEDLESS_CPU_CXX_CHECK "SEEDLESS_CPU_${SEEDLESS_CPU_ID}_${SEEDLESS_CPU_MODE}_CXX")
    check_c_compiler_flag("${SEEDLESS_CPU_FLAG}" "${SEEDLESS_CPU_C_CHECK}")
    check_cxx_compiler_flag("${SEEDLESS_CPU_FLAG}" "${SEEDLESS_CPU_CXX_CHECK}")
    if(NOT ${SEEDLESS_CPU_C_CHECK} OR NOT ${SEEDLESS_CPU_CXX_CHECK})
        if(SEEDLESS_ARM_CPU STREQUAL "cortex-a55")
            string(REPLACE "cortex-a55" "cortex_a55" SEEDLESS_CPU_FLAG_ALT
                   "${SEEDLESS_CPU_FLAG}")
            check_c_compiler_flag("${SEEDLESS_CPU_FLAG_ALT}"
                                  SEEDLESS_CPU_cortex_a55_${SEEDLESS_CPU_MODE}_ALT_C)
            check_cxx_compiler_flag("${SEEDLESS_CPU_FLAG_ALT}"
                                    SEEDLESS_CPU_cortex_a55_${SEEDLESS_CPU_MODE}_ALT_CXX)
            if(SEEDLESS_CPU_cortex_a55_${SEEDLESS_CPU_MODE}_ALT_C AND
               SEEDLESS_CPU_cortex_a55_${SEEDLESS_CPU_MODE}_ALT_CXX)
                set(SEEDLESS_CPU_C_CHECK SEEDLESS_CPU_cortex_a55_${SEEDLESS_CPU_MODE}_ALT_C)
                set(SEEDLESS_CPU_CXX_CHECK SEEDLESS_CPU_cortex_a55_${SEEDLESS_CPU_MODE}_ALT_CXX)
                set(SEEDLESS_CPU_FLAG "${SEEDLESS_CPU_FLAG_ALT}")
            endif()
        endif()
    endif()
    if(NOT ${SEEDLESS_CPU_C_CHECK} OR NOT ${SEEDLESS_CPU_CXX_CHECK})
        message(FATAL_ERROR "Unsupported AArch64 CPU flag: ${SEEDLESS_CPU_FLAG}")
    endif()
    set(SEEDLESS_ARM_CPU_FLAG "${SEEDLESS_CPU_FLAG}")

    foreach(target IN ITEMS recon_fn seedlessds_libretro
            seedless_lua seedless_unrar seedless_lzma zlibstatic zlib)
        if(NOT TARGET ${target})
            continue()
        endif()
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:${SEEDLESS_ARM_CPU_FLAG}>")
    endforeach()
endif()

# Persisted native blocks embed this compiler's instruction selection and JIT
# emitter ABI. Re-key their cache whenever any emitter, machine-layout header,
# compiler, target tuning, or core build flags change.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64" OR
   CMAKE_C_COMPILER_TARGET MATCHES "aarch64")
    file(GLOB_RECURSE SEEDLESS_JIT_ABI_SOURCES CONFIGURE_DEPENDS
        "${SEEDLESS_CORE_DIR}/src/*.c"
        "${SEEDLESS_CORE_DIR}/src/*.h"
        "${SEEDLESS_CORE_DIR}/src/*.S"
        "${CMAKE_CURRENT_SOURCE_DIR}/libretro/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/libretro/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/libretro/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libretro/*.S")
    list(APPEND SEEDLESS_JIT_ABI_SOURCES
        "${SEEDLESS_CORE_DIR}/src/cpu/arm.h"
        "${SEEDLESS_CORE_DIR}/src/cpu/bus.h"
        "${SEEDLESS_CORE_DIR}/src/core/nds_state.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
        "${SEEDLESS_CORE_DIR}/src/CMakeLists.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${SEEDLESS_JIT_ABI_SOURCES})
    set(SEEDLESS_JIT_ABI_INPUT
        "${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_TARGET};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_TARGET};${CMAKE_ASM_COMPILER_ID};${CMAKE_ASM_COMPILER_VERSION};${CMAKE_ASM_COMPILER};${CMAKE_LINKER};${CMAKE_SYSTEM_PROCESSOR};${SEEDLESS_ARM_CPU};${SEEDLESS_ARM_CPU_FLAG};${SEEDLESS_ARM_USE_NATIVE_CPU};${SEEDLESS_IPO_ACTIVE};${SEEDLESS_ENABLE_IPO};${SEEDLESS_JIT_PROFILE};${CMAKE_BUILD_TYPE};${CMAKE_C_FLAGS};${CMAKE_C_FLAGS_DEBUG};${CMAKE_C_FLAGS_RELEASE};${CMAKE_C_FLAGS_RELWITHDEBINFO};${CMAKE_C_FLAGS_MINSIZEREL};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_CXX_FLAGS_RELWITHDEBINFO};${CMAKE_CXX_FLAGS_MINSIZEREL};${CMAKE_ASM_FLAGS};${CMAKE_ASM_FLAGS_DEBUG};${CMAKE_ASM_FLAGS_RELEASE};${CMAKE_ASM_FLAGS_RELWITHDEBINFO};${CMAKE_ASM_FLAGS_MINSIZEREL};${CMAKE_SHARED_LINKER_FLAGS};${CMAKE_SHARED_LINKER_FLAGS_DEBUG};${CMAKE_SHARED_LINKER_FLAGS_RELEASE};${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO};${CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL};${CMAKE_MODULE_LINKER_FLAGS};${CMAKE_MODULE_LINKER_FLAGS_DEBUG};${CMAKE_MODULE_LINKER_FLAGS_RELEASE};${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO};${CMAKE_MODULE_LINKER_FLAGS_MINSIZEREL};${CMAKE_EXE_LINKER_FLAGS};${CMAKE_EXE_LINKER_FLAGS_DEBUG};${CMAKE_EXE_LINKER_FLAGS_RELEASE};${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO};${CMAKE_EXE_LINKER_FLAGS_MINSIZEREL}")
    foreach(source IN LISTS SEEDLESS_JIT_ABI_SOURCES)
        file(SHA256 "${source}" source_hash)
        string(APPEND SEEDLESS_JIT_ABI_INPUT ";${source}:${source_hash}")
    endforeach()
    string(SHA256 SEEDLESS_JIT_ABI_HASH "${SEEDLESS_JIT_ABI_INPUT}")
    string(SUBSTRING "${SEEDLESS_JIT_ABI_HASH}" 0 16 SEEDLESS_JIT_ABI_HASH_SHORT)
    target_compile_definitions(recon_fn PRIVATE
        SEEDLESS_JIT_NATIVE_CACHE_ABI=0x${SEEDLESS_JIT_ABI_HASH_SHORT}ULL)
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
