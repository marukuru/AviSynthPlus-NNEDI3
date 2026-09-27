cmake_minimum_required(VERSION 3.16)
project(nnedi3 VERSION 0.9.4.69 LANGUAGES CXX ASM)

include(TestBigEndian)
test_big_endian(NNEDI3_BIG_ENDIAN)
if(NNEDI3_BIG_ENDIAN)
    message(FATAL_ERROR "The embedded NNEDI3 weights require a little-endian target.")
endif()

if(WIN32)
    message(FATAL_ERROR "Use nnedi3.sln for the Windows MASM build.")
endif()

set(AVISYNTH_SOURCE_DIR "" CACHE PATH "Optional path to an AviSynth+ source checkout")
find_path(AVISYNTH_INCLUDE_DIR avisynth.h
    HINTS "${AVISYNTH_SOURCE_DIR}/avs_core/include"
    PATH_SUFFIXES avisynth)
if(NOT AVISYNTH_INCLUDE_DIR)
    message(FATAL_ERROR "Install AviSynth+ headers or set AVISYNTH_SOURCE_DIR / AVISYNTH_INCLUDE_DIR.")
endif()
find_package(Threads REQUIRED)
include(GNUInstallDirs)

configure_file(cmake/weights.S.in weights.S @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/weights.S" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/nnedi3/binary1.bin")
add_library(nnedi3 MODULE nnedi3/nnedi3.cpp nnedi3/PlanarFrame.cpp
    nnedi3/Parallel.cpp nnedi3/PortableKernels.cpp "${CMAKE_CURRENT_BINARY_DIR}/weights.S")
target_include_directories(nnedi3 SYSTEM PRIVATE "${AVISYNTH_INCLUDE_DIR}")
target_compile_features(nnedi3 PRIVATE cxx_std_11)
target_link_libraries(nnedi3 PRIVATE Threads::Threads)
# The legacy kernels store quantized int16 weights in float allocations.
target_compile_options(nnedi3 PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-strict-aliasing;-ffp-contract=off>")
target_link_options(nnedi3 PRIVATE -Wl,--no-undefined)
set_target_properties(nnedi3 PROPERTIES PREFIX "lib" OUTPUT_NAME "nnedi3.${PROJECT_VERSION}" CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
# Assemble the original tuned kernels as ELF, with generated System V / Win64
# ABI adapters. ISA selection stays in baseline C++ code; no -march=native.
set(NNEDI3_X64 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NNEDI3_X64 ON)
endif()
option(NNEDI3_X86_ASM "Build all x64 SIMD optimization modes (requires NASM and Python 3)" ${NNEDI3_X64})
if(NNEDI3_X86_ASM)
    if(NOT NNEDI3_X64)
        message(FATAL_ERROR "NNEDI3_X86_ASM requires an x86-64 target.")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    enable_language(ASM_NASM)
    set(asm_dir "${CMAKE_CURRENT_BINARY_DIR}/asm")
    set(asm_sources
        "${asm_dir}/nnedi3_asm_x64.asm"
        "${asm_dir}/nnedi3_asm_FMA_x64.asm"
        "${asm_dir}/nnedi3_asm_AVX512_x64.asm"
        "${asm_dir}/KernelWrappers.cpp")
    add_custom_command(OUTPUT ${asm_sources}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/cmake/masm_to_nasm.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/nnedi3" "${asm_dir}"
        DEPENDS cmake/masm_to_nasm.py nnedi3/nnedi3.cpp
            nnedi3/nnedi3_asm_x64.asm nnedi3/nnedi3_asm_FMA_x64.asm nnedi3/nnedi3_asm_AVX512_x64.asm
        VERBATIM)
    add_library(nnedi3_asm OBJECT ${asm_sources})
    set_target_properties(nnedi3_asm PROPERTIES POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    target_compile_features(nnedi3_asm PRIVATE cxx_std_11)
    target_link_libraries(nnedi3 PRIVATE nnedi3_asm)
    target_compile_definitions(nnedi3 PRIVATE NNEDI3_X86_ASM=1)
endif()

set(AVISYNTH_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/avisynth" CACHE PATH "AviSynth+ plugin install directory")
install(TARGETS nnedi3 LIBRARY DESTINATION "${AVISYNTH_PLUGIN_DIR}")

option(NNEDI3_BUILD_TESTS "Build integration tests (requires the AviSynth+ runtime)" OFF)
if(NNEDI3_BUILD_TESTS)
    enable_testing()
    find_library(AVISYNTH_LIBRARY avisynth)
    if(NOT AVISYNTH_LIBRARY)
        message(FATAL_ERROR "NNEDI3_BUILD_TESTS requires the AviSynth+ runtime library.")
    endif()
    add_executable(nnedi3_integration tests/integration.cpp nnedi3/PortableKernels.cpp)
    if(NNEDI3_X86_ASM)
        target_compile_definitions(nnedi3_integration PRIVATE NNEDI3_X86_ASM=1)
    endif()
    set_target_properties(nnedi3_integration PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    target_include_directories(nnedi3_integration SYSTEM PRIVATE "${AVISYNTH_INCLUDE_DIR}")
    target_compile_features(nnedi3_integration PRIVATE cxx_std_11)
    target_link_libraries(nnedi3_integration PRIVATE "${AVISYNTH_LIBRARY}" Threads::Threads)
    add_test(NAME integration COMMAND nnedi3_integration $<TARGET_FILE:nnedi3>)
    set_tests_properties(integration PROPERTIES TIMEOUT 180)
    add_executable(nnedi3_kernels tests/kernels.cpp nnedi3/PortableKernels.cpp nnedi3/Parallel.cpp)
    target_include_directories(nnedi3_kernels PRIVATE nnedi3)
    target_include_directories(nnedi3_kernels SYSTEM PRIVATE "${AVISYNTH_INCLUDE_DIR}")
    target_compile_features(nnedi3_kernels PRIVATE cxx_std_11)
    target_compile_options(nnedi3_kernels PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(nnedi3_kernels PRIVATE Threads::Threads)
    add_test(NAME kernels_and_workers COMMAND nnedi3_kernels)
    set_tests_properties(kernels_and_workers PROPERTIES TIMEOUT 30)
endif()
