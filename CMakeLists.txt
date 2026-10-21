cmake_minimum_required(VERSION 3.24)
project(EldenRingDLSS VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options(/W4 /permissive- /utf-8)

# NVIDIA's DLSS SDK is downloaded by tools/fetch_dlss_sdk.ps1 and stays under NVIDIA's license.
set(DLSS_SDK_DIR ${CMAKE_SOURCE_DIR}/third_party/dlss CACHE PATH "NVIDIA DLSS SDK directory")
if(NOT EXISTS ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib)
  message(FATAL_ERROR "DLSS SDK not found in ${DLSS_SDK_DIR}. Run tools/fetch_dlss_sdk.ps1 first.")
endif()
# NVIDIA's NVAPI (MIT, Reflex low-latency sleep), downloaded by tools/fetch_nvapi.ps1.
set(NVAPI_DIR ${CMAKE_SOURCE_DIR}/third_party/nvapi)
if(NOT EXISTS ${NVAPI_DIR}/amd64/nvapi64.lib)
  message(FATAL_ERROR "NVAPI not found in ${NVAPI_DIR}. Run tools/fetch_nvapi.ps1 first.")
endif()
# NVIDIA's Streamline SDK (MIT; DLSS Frame Generation), downloaded by tools/fetch_streamline.ps1. Only its headers are
# used at build time: the add-on loads sl.interposer.dll at run time.
set(STREAMLINE_DIR ${CMAKE_SOURCE_DIR}/third_party/streamline)
if(NOT EXISTS ${STREAMLINE_DIR}/include/sl.h)
  message(FATAL_ERROR "Streamline not found in ${STREAMLINE_DIR}. Run tools/fetch_streamline.ps1 first.")
endif()

# ReShade's state tracking helper (BSD-3-Clause OR MIT).
add_library(reshade_utils STATIC third_party/reshade/state_tracking.cpp)
target_include_directories(reshade_utils PUBLIC third_party/reshade)
target_compile_options(reshade_utils PRIVATE /W0)

# Shaders are compiled to DXBC headers with the Windows SDK's fxc: shader(<symbol> <file> <profile> <entry> [defines])
# writes ${SHADER_DIR}/<symbol>.h with the bytecode array <symbol>. dxil_shader() does the same with the SDK's dxc
# for shaders that join the game's own (DXIL) shaders in one pipeline; dxc signs them with the dxil.dll next to it.
find_program(FXC fxc REQUIRED)
get_filename_component(SDK_BIN_DIR ${FXC} DIRECTORY)
find_program(DXC dxc HINTS ${SDK_BIN_DIR} NO_DEFAULT_PATH)
find_program(DXC dxc REQUIRED)
set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
set(SHADER_INCLUDES src/wind_common.hlsli)
function(shader symbol file profile entry)
  set(defines)
  foreach(define ${ARGN})
    list(APPEND defines /D ${define})
  endforeach()
  set(header ${SHADER_DIR}/${symbol}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND ${FXC} /nologo /T ${profile} /E ${entry} /O3 ${defines} /Vn ${symbol} /Fh ${header}
            ${CMAKE_SOURCE_DIR}/src/${file}
    DEPENDS src/${file} ${SHADER_INCLUDES}
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${header} PARENT_SCOPE)
endfunction()
function(dxil_shader symbol file profile entry)
  set(defines)
  foreach(define ${ARGN})
    list(APPEND defines -D ${define})
  endforeach()
  set(header ${SHADER_DIR}/${symbol}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND ${DXC} -nologo -T ${profile} -E ${entry} -O3 -HV 2018 ${defines} -Vn ${symbol} -Fh ${header}
            ${CMAKE_SOURCE_DIR}/src/${file}
    DEPENDS src/${file} ${SHADER_INCLUDES}
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${header} PARENT_SCOPE)
endfunction()

# dxil_library(<symbol> <file>): a DXIL library of ray tracing shaders (lib_6_3, the game's model) for rt_pipeline.cpp.
function(dxil_library symbol file)
  set(header ${SHADER_DIR}/${symbol}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND ${DXC} -nologo -T lib_6_3 -O3 -HV 2018 -Vn ${symbol} -Fh ${header} ${CMAKE_SOURCE_DIR}/src/${file}
    DEPENDS src/${file} src/blue_noise.hlsli
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${header} PARENT_SCOPE)
endfunction()

shader(kVegetationMaskCs vegetation_mask.hlsl cs_5_0 main)
shader(kWindResolveVs wind_resolve.hlsl vs_5_0 VSMain)
shader(kWindResolvePs wind_resolve.hlsl ps_5_0 PSMain)
shader(kWindResolveArgsCs wind_resolve.hlsl cs_5_0 ArgsMain)
shader(kWindPatchCs wind_patch.hlsl cs_5_0 main)
shader(kWindMergeVs wind_merge.hlsl vs_5_0 VSMain)
shader(kWindMergePs wind_merge.hlsl ps_5_0 PSMain)
shader(kStretchVs stretch.hlsl vs_5_0 VSMain)
shader(kStretchPsColor stretch.hlsl ps_5_0 PSColor)
shader(kStretchPsDepth stretch.hlsl ps_5_0 PSDepth)
shader(kWaterSsrVs water_ssr.hlsl vs_5_0 VSMain)
shader(kWaterSsrPs water_ssr.hlsl ps_5_0 PSMain)
shader(kRrGuidesVs rr_guides.hlsl vs_5_0 VSMain)
shader(kRrGuidesPs rr_guides.hlsl ps_5_0 PSMain)
shader(kAoUpscaleVs ao_upscale.hlsl vs_5_0 VSMain)
shader(kAoUpscalePs ao_upscale.hlsl ps_5_0 PSMain)
shader(kAoUpscalePsFull ao_upscale.hlsl ps_5_0 PSFull)
shader(kTreeDumpCs tree_dump.hlsl cs_5_0 main)
shader(kTreeWindPatchCs tree_wind.hlsl cs_5_0 Patch)
shader(kTreeWindClearCs tree_wind.hlsl cs_5_0 Clear)
shader(kTreeWindAdvanceCs tree_wind.hlsl cs_5_0 Advance)
shader(kTreeWindVerifyCs tree_wind.hlsl cs_5_0 Verify)
shader(kObjectBlurVs object_blur.hlsl vs_5_0 VSMain)
shader(kObjectBlurPs object_blur.hlsl ps_5_0 PSMain)
shader(kSkinMotionLookupCs skin_motion.hlsl cs_5_0 Lookup)
shader(kSkinMotionResolveCs skin_motion.hlsl cs_5_0 Resolve)
shader(kSkinMotionClearCs skin_motion.hlsl cs_5_0 Clear)
shader(kSkinMotionAdvanceCs skin_motion.hlsl cs_5_0 Advance)
shader(kUiAlphaCs ui_alpha.hlsl cs_5_0 main)
# Ported grass shaders: per material a fused (default), an unfused and a static vertex shader (the latter two for cost
# measurements), and the alpha-tested pixel shaders.
foreach(variant FUSED UNFUSED STATIC)
  set(suffix "")
  set(extra "")
  if(NOT variant STREQUAL "FUSED")
    string(TOLOWER ${variant} lower)
    string(SUBSTRING ${lower} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${lower} 1 -1 rest)
    set(suffix "${first}${rest}")
    set(extra ${variant}=1)
  endif()
  shader(kWindPortVsGrass2${suffix} wind_port.hlsl vs_5_0 VSMain PHASE_COMP=y BEND_COMP=z ${extra})
  shader(kWindPortVsVesitation${suffix} wind_port.hlsl vs_5_0 VSMain PHASE_COMP=x BEND_COMP=w ${extra})
  shader(kWindPortVsGrass2Lod1${suffix} wind_port.hlsl vs_5_0 VSMain LOD1=1 ${extra})
endforeach()
shader(kWindPortPs wind_port.hlsl ps_5_0 PSMain)
shader(kWindPortPsDiscard wind_port.hlsl ps_5_0 PSDiscard)
shader(kWindPortPsGrass2 wind_port.hlsl ps_5_0 PSMain ALPHA_TEST=1)
shader(kWindPortPsVesitation wind_port.hlsl ps_5_0 PSMain ALPHA_TEST=2)
shader(kWindPortPsLod1 wind_port.hlsl ps_5_0 PSMain ALPHA_TEST=3)
# Replacement pixel shaders for the grass depth prepasses.
dxil_shader(kWindPrepassPsGrass2 wind_prepass.hlsl ps_6_0 PSMain ALPHA_TEST=1 PHASE_COMP=y BEND_COMP=z)
dxil_shader(kWindPrepassPsVesitation wind_prepass.hlsl ps_6_0 PSMain ALPHA_TEST=2 PHASE_COMP=x BEND_COMP=w)
# Ray generation shaders added to the game's ray tracing pipeline.
dxil_library(kRtRayGen rt_raygen.hlsl)

configure_file(src/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)

set(ERDLSS_RUNTIME_SOURCES src/descriptors.cpp src/dlss.cpp src/wind.cpp src/wind_port.cpp
            src/tree_dump.cpp src/engine_resolution.cpp src/render_scale.cpp src/stretch.cpp src/water_ssr.cpp src/taa_jitter.cpp src/rr_guides.cpp src/ao_upscale.cpp
            src/hw_breakpoints.cpp src/drs_probe.cpp src/rt_pipeline.cpp src/skin_motion.cpp src/dred.cpp src/object_blur.cpp src/reflex.cpp src/gpu_profiler.cpp src/frame_generation.cpp src/pre_lists.cpp)
add_library(EldenRingDLSS SHARED src/addon.cpp ${ERDLSS_RUNTIME_SOURCES} ${CMAKE_BINARY_DIR}/version.rc ${SHADER_HEADERS})
target_include_directories(EldenRingDLSS PRIVATE src third_party/imgui ${SHADER_DIR})
target_include_directories(EldenRingDLSS SYSTEM PRIVATE ${DLSS_SDK_DIR}/include ${NVAPI_DIR} ${STREAMLINE_DIR}/include)
target_link_libraries(EldenRingDLSS PRIVATE reshade_utils ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib
                      ${NVAPI_DIR}/amd64/nvapi64.lib d3d12 dxgi)
set_target_properties(EldenRingDLSS PROPERTIES SUFFIX ".addon64")

# Development tool: times DLSS outside the game (tools/dlss_bench/dlss_bench.cpp).
add_executable(dlss_bench tools/dlss_bench/dlss_bench.cpp)
target_include_directories(dlss_bench SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
target_link_libraries(dlss_bench PRIVATE ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib d3d12 dxgi)

# Development tool: runs frame generation outside the game in a window of its own (tools/fg_smoke/fg_smoke.cpp).
add_executable(fg_smoke tools/fg_smoke/fg_smoke.cpp src/frame_generation.cpp src/dlss.cpp ${SHADER_DIR}/kUiAlphaCs.h)
target_include_directories(fg_smoke PRIVATE src third_party/reshade ${SHADER_DIR})
target_include_directories(fg_smoke SYSTEM PRIVATE ${STREAMLINE_DIR}/include ${DLSS_SDK_DIR}/include)
target_link_libraries(fg_smoke PRIVATE ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib d3d12 dxgi)

# Opt-in regression checks exercise addon helpers without loading or controlling the game.
option(ERDLSS_BUILD_TESTS "Build standalone addon regression checks" OFF)
if(ERDLSS_BUILD_TESTS)
  enable_testing()
  add_executable(erdlss_regressions tests/regressions.cpp ${ERDLSS_RUNTIME_SOURCES} ${SHADER_HEADERS})
  target_include_directories(erdlss_regressions PRIVATE src third_party/imgui ${SHADER_DIR})
  target_include_directories(erdlss_regressions SYSTEM PRIVATE ${DLSS_SDK_DIR}/include ${NVAPI_DIR}
                             ${STREAMLINE_DIR}/include)
  target_link_libraries(erdlss_regressions PRIVATE reshade_utils ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib
                        ${NVAPI_DIR}/amd64/nvapi64.lib d3d12 dxgi)
  add_test(NAME addon_regressions COMMAND erdlss_regressions)
endif()
