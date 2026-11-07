cmake_minimum_required(VERSION 3.21)
project(BevelAlpha VERSION 1.3.0 LANGUAGES CXX)

option(BEVEL_BUILD_PLUGIN "Build the Adobe Windows effect" ON)
option(BEVEL_BUILD_GPU "Add the Premiere CUDA rendering path" OFF)
set(AE_SDK_ROOT "" CACHE PATH "Extracted Adobe SDK root or its Examples folder")
set(PR_SDK_ROOT "" CACHE PATH "Extracted Premiere C++ SDK root or its Examples folder")
set(BEVEL_NVRTC_DLL "" CACHE FILEPATH "Absolute path to NVIDIA's Windows x64 NVRTC DLL")

include(CTest)
if(BUILD_TESTING)
    add_executable(BevelRenderTests BevelRenderTests.cpp)
    target_compile_features(BevelRenderTests PRIVATE cxx_std_17)
    add_test(NAME BevelRenderTests COMMAND BevelRenderTests)
endif()

if(NOT BEVEL_BUILD_PLUGIN)
    return()
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Adobe plug-in target requires Windows x64.")
endif()

if(NOT AE_SDK_ROOT)
    message(FATAL_ERROR "Set AE_SDK_ROOT to the extracted Adobe After Effects SDK. See README.md.")
endif()

set(ae_examples "${AE_SDK_ROOT}")
if(EXISTS "${ae_examples}/Examples/Headers/AE_Effect.h")
    set(ae_examples "${ae_examples}/Examples")
endif()
set(ae_headers "${ae_examples}/Headers")
set(ae_resources "${ae_examples}/Resources")
find_file(pipl_tool NAMES PiPLtool.exe PiPLTool.exe PATHS "${ae_resources}"
    NO_DEFAULT_PATH NO_CACHE)

if(NOT EXISTS "${ae_headers}/AE_Effect.h" OR NOT pipl_tool)
    message(FATAL_ERROR "AE_SDK_ROOT must contain the SDK's Headers and Resources/PiPLtool.exe, directly or under Examples.")
endif()

if(NOT MSVC AND NOT MINGW)
    message(FATAL_ERROR "Use Visual Studio or MinGW-w64 for the Adobe plug-in target.")
endif()

if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(x64|x86_64)$")
    message(FATAL_ERROR "Select the x64 toolset, not the ARM64 toolset.")
endif()

enable_language(RC)
set(pipl_source "${CMAKE_CURRENT_SOURCE_DIR}/BevelAlphaPiPL.r")
set(pipl_output "${CMAKE_CURRENT_BINARY_DIR}/BevelAlphaPiPL.rc")
add_custom_command(OUTPUT "${pipl_output}"
    COMMAND "${CMAKE_COMMAND}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}"
        "-DMSVC=${MSVC}"
        "-DHEADERS=${ae_headers}"
        "-DPIPL_TOOL=${pipl_tool}"
        "-DSOURCE=${pipl_source}"
        "-DOUTPUT=${pipl_output}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CompilePiPL.cmake"
    DEPENDS "${pipl_source}" PluginInfo.h CompilePiPL.cmake
        "${ae_headers}/AEConfig.h" "${ae_headers}/AE_EffectVers.h" "${pipl_tool}"
    VERBATIM)

add_library(BevelAlpha MODULE BevelAlpha.cpp "${pipl_output}")
target_compile_features(BevelAlpha PRIVATE cxx_std_17)
target_compile_definitions(BevelAlpha PRIVATE _WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN PF_DEEP_COLOR_AWARE)
target_include_directories(BevelAlpha SYSTEM PRIVATE
    "${ae_headers}" "${ae_headers}/SP" "${ae_examples}/Util" "${ae_resources}")
set_target_properties(BevelAlpha PROPERTIES PREFIX "" SUFFIX ".aex"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MINGW)
    target_link_options(BevelAlpha PRIVATE -static)
endif()

if(BEVEL_BUILD_GPU)
    set(pr_examples "${PR_SDK_ROOT}")
    if(EXISTS "${pr_examples}/Examples/Headers/PrSDKGPUFilter.h")
        set(pr_examples "${pr_examples}/Examples")
    endif()
    set(pr_headers "${pr_examples}/Headers")
    if(NOT EXISTS "${pr_headers}/PrSDKGPUFilter.h" OR NOT EXISTS "${BEVEL_NVRTC_DLL}")
        message(FATAL_ERROR "The CUDA path needs PR_SDK_ROOT and BEVEL_NVRTC_DLL. See README.md.")
    endif()

    add_executable(CompileCuda CompileCuda.cpp)
    target_compile_features(CompileCuda PRIVATE cxx_std_17)
    target_compile_definitions(CompileCuda PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MINGW)
        target_link_options(CompileCuda PRIVATE -static)
    endif()
    set(cuda_source "${CMAKE_CURRENT_SOURCE_DIR}/BevelCuda.cu")
    set(cuda_ptx "${CMAKE_CURRENT_BINARY_DIR}/BevelCudaPtx.h")
    add_custom_command(OUTPUT "${cuda_ptx}"
        COMMAND CompileCuda "${BEVEL_NVRTC_DLL}" "${cuda_source}" "${cuda_ptx}"
        DEPENDS CompileCuda "${cuda_source}" BevelCudaParams.h "${BEVEL_NVRTC_DLL}"
        VERBATIM)
    target_sources(BevelAlpha PRIVATE BevelGpu.cpp "${cuda_ptx}")
    target_include_directories(BevelAlpha PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    set_source_files_properties(BevelGpu.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${pr_headers};${pr_headers}/SP")
endif()
