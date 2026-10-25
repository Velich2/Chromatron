cmake_minimum_required(VERSION 3.20)
project(Chromatron VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    endif()
endif()

option(CHROMATRON_BUILD_OFX "Build the OpenFX plug-in (DaVinci Resolve)" ON)
option(CHROMATRON_BUILD_AE "Build the After Effects plug-in (needs AE_SDK_DIR)" OFF)
option(CHROMATRON_BUILD_TOOLS "Build the command-line renderer and tests" ON)

if(APPLE)
    option(CHROMATRON_WITH_METAL "Metal backend" ON)
    option(CHROMATRON_WITH_OPENCL "OpenCL backend" OFF)
    set(CHROMATRON_WITH_CUDA OFF)
else()
    set(CHROMATRON_WITH_METAL OFF)
    option(CHROMATRON_WITH_OPENCL "OpenCL backend" ON)
    option(CHROMATRON_WITH_CUDA "CUDA backend (needs the CUDA toolkit)" ON)
endif()

# Plug-ins load into foreign processes: keep our symbols private.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# Embedded GPU program sources (OpenCL / Metal are compiled at runtime).
# ---------------------------------------------------------------------------
set(KERNEL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${GENERATED_DIR}/ChromatronKernelSources.h
    COMMAND ${CMAKE_COMMAND}
        -DKERNEL_HEADER=${KERNEL_DIR}/ChromatronKernels.h
        -DOPENCL_WRAPPERS=${KERNEL_DIR}/ChromatronKernels.cl
        -DMETAL_WRAPPERS=${KERNEL_DIR}/ChromatronKernels.metal
        -DOUTPUT=${GENERATED_DIR}/ChromatronKernelSources.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedKernels.cmake
    DEPENDS ${KERNEL_DIR}/ChromatronKernels.h ${KERNEL_DIR}/ChromatronKernels.cl
            ${KERNEL_DIR}/ChromatronKernels.metal ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedKernels.cmake
    COMMENT "Embedding Chromatron GPU kernels")
add_custom_target(chromatron_kernel_sources DEPENDS ${GENERATED_DIR}/ChromatronKernelSources.h)

# ---------------------------------------------------------------------------
# Core: model, pipeline and backends.
# ---------------------------------------------------------------------------
add_library(chromatron_core STATIC
    src/core/Chromatron.cpp
    src/backends/CpuBackend.cpp)
add_dependencies(chromatron_core chromatron_kernel_sources)
target_include_directories(chromatron_core PUBLIC src PRIVATE ${GENERATED_DIR})
find_package(Threads REQUIRED)
target_link_libraries(chromatron_core PUBLIC Threads::Threads)

if(CHROMATRON_WITH_OPENCL)
    find_package(OpenCL)
    if(OpenCL_FOUND)
        target_sources(chromatron_core PRIVATE src/backends/OpenCLBackend.cpp)
        target_link_libraries(chromatron_core PUBLIC OpenCL::OpenCL)
        target_compile_definitions(chromatron_core PUBLIC CHROMATRON_HAS_OPENCL=1)
        if(MSVC)
            # Load OpenCL.dll only when OpenCL is used, so the plug-ins still load on
            # machines without an OpenCL driver.
            target_link_options(chromatron_core INTERFACE /DELAYLOAD:OpenCL.dll)
            target_link_libraries(chromatron_core PUBLIC delayimp)
        endif()
    else()
        message(WARNING "OpenCL not found: OpenCL backend disabled")
    endif()
endif()

if(CHROMATRON_WITH_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        # Must be set before enable_language(), which otherwise picks the compiler's default.
        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
            execute_process(COMMAND "${CMAKE_CUDA_COMPILER}" --version OUTPUT_VARIABLE NVCC_VERSION_TEXT)
            if(NVCC_VERSION_TEXT MATCHES "release ([0-9]+)\\." AND CMAKE_MATCH_1 GREATER_EQUAL 13)
                # CUDA 13 dropped Pascal and Volta: Turing .. Blackwell, plus PTX for newer GPUs.
                set(CMAKE_CUDA_ARCHITECTURES "75-real;80-real;86-real;89-real;90-real;100-real;120")
            else()
                # Pascal .. Ada/Hopper, plus PTX for newer GPUs.
                set(CMAKE_CUDA_ARCHITECTURES "61-real;70-real;75-real;80-real;86-real;89-real;90")
            endif()
        endif()
        enable_language(CUDA)
        target_sources(chromatron_core PRIVATE src/backends/CudaBackend.cu)
        # chromatron_core already exists, so it did not pick up CMAKE_CUDA_ARCHITECTURES.
        set_target_properties(chromatron_core PROPERTIES CUDA_RUNTIME_LIBRARY Static
                                                         CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        find_package(CUDAToolkit REQUIRED)
        target_link_libraries(chromatron_core PUBLIC CUDA::cudart_static)
        target_compile_definitions(chromatron_core PUBLIC CHROMATRON_HAS_CUDA=1)
    else()
        message(WARNING "CUDA compiler not found: CUDA backend disabled")
    endif()
endif()

if(CHROMATRON_WITH_METAL)
    target_sources(chromatron_core PRIVATE src/backends/MetalBackend.mm)
    set_source_files_properties(src/backends/MetalBackend.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(chromatron_core PUBLIC "-framework Metal" "-framework Foundation")
    target_compile_definitions(chromatron_core PUBLIC CHROMATRON_HAS_METAL=1)
endif()

if(MSVC)
    target_compile_options(chromatron_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /fp:fast>)
else()
    target_compile_options(chromatron_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
endif()

if(CHROMATRON_BUILD_OFX)
    add_subdirectory(src/ofx)
endif()
if(CHROMATRON_BUILD_AE)
    add_subdirectory(src/ae)
endif()
if(CHROMATRON_BUILD_TOOLS)
    enable_testing()
    add_executable(chromatron_cli tools/chromatron_cli.cpp)
    target_link_libraries(chromatron_cli PRIVATE chromatron_core)
    add_executable(chromatron_tests tests/test_core.cpp)
    target_link_libraries(chromatron_tests PRIVATE chromatron_core)
    if(CHROMATRON_WITH_METAL)
        target_sources(chromatron_tests PRIVATE tests/MetalHarness.mm)
        set_source_files_properties(tests/MetalHarness.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
    add_test(NAME chromatron_tests COMMAND chromatron_tests)
endif()
