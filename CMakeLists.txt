cmake_minimum_required(VERSION 3.24)
project(EldenRingDLSS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options(/W4 /permissive- /utf-8)

# NVIDIA's DLSS SDK is downloaded by tools/fetch_dlss_sdk.ps1 and stays under NVIDIA's license.
set(DLSS_SDK_DIR ${CMAKE_SOURCE_DIR}/third_party/dlss)
if(NOT EXISTS ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib)
  message(FATAL_ERROR "DLSS SDK not found in ${DLSS_SDK_DIR}. Run tools/fetch_dlss_sdk.ps1 first.")
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
shader(kTreeDumpCs tree_dump.hlsl cs_5_0 main)
shader(kTreeWindPatchCs tree_wind.hlsl cs_5_0 Patch)
shader(kTreeWindClearCs tree_wind.hlsl cs_5_0 Clear)
shader(kTreeWindAdvanceCs tree_wind.hlsl cs_5_0 Advance)
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

add_library(EldenRingDLSS SHARED src/addon.cpp src/descriptors.cpp src/dlss.cpp src/wind.cpp src/wind_port.cpp
            src/tree_dump.cpp src/engine_resolution.cpp src/render_scale.cpp src/post_resolution.cpp src/stretch.cpp
            src/hw_breakpoints.cpp src/drs_probe.cpp ${SHADER_HEADERS})
target_include_directories(EldenRingDLSS PRIVATE src third_party/imgui ${SHADER_DIR})
target_include_directories(EldenRingDLSS SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
target_link_libraries(EldenRingDLSS PRIVATE reshade_utils ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib d3d12)
set_target_properties(EldenRingDLSS PROPERTIES SUFFIX ".addon64")
