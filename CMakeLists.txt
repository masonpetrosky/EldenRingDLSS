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

# Compute shaders are compiled to DXBC headers with the Windows SDK's fxc.
find_program(FXC fxc REQUIRED)
set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
add_custom_command(
  OUTPUT ${SHADER_DIR}/vegetation_mask_cs.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
  COMMAND ${FXC} /nologo /T cs_5_0 /E main /O3 /Vn kVegetationMaskCs /Fh ${SHADER_DIR}/vegetation_mask_cs.h
          ${CMAKE_SOURCE_DIR}/src/vegetation_mask.hlsl
  DEPENDS src/vegetation_mask.hlsl
  VERBATIM)
add_custom_command(
  OUTPUT ${SHADER_DIR}/wind_resolve_vs.h ${SHADER_DIR}/wind_resolve_ps.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
  COMMAND ${FXC} /nologo /T vs_5_0 /E VSMain /O3 /Vn kWindResolveVs /Fh ${SHADER_DIR}/wind_resolve_vs.h
          ${CMAKE_SOURCE_DIR}/src/wind_resolve.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E PSMain /O3 /Vn kWindResolvePs /Fh ${SHADER_DIR}/wind_resolve_ps.h
          ${CMAKE_SOURCE_DIR}/src/wind_resolve.hlsl
  DEPENDS src/wind_resolve.hlsl
  VERBATIM)
add_custom_command(
  OUTPUT ${SHADER_DIR}/wind_patch_cs.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
  COMMAND ${FXC} /nologo /T cs_5_0 /E main /O3 /Vn kWindPatchCs /Fh ${SHADER_DIR}/wind_patch_cs.h
          ${CMAKE_SOURCE_DIR}/src/wind_patch.hlsl
  DEPENDS src/wind_patch.hlsl
  VERBATIM)

add_custom_command(
  OUTPUT ${SHADER_DIR}/wind_port_vs_grass2.h ${SHADER_DIR}/wind_port_vs_vesitation.h
         ${SHADER_DIR}/wind_port_vs_grass2_lod1.h ${SHADER_DIR}/wind_port_ps.h ${SHADER_DIR}/wind_port_ps_grass2.h
         ${SHADER_DIR}/wind_port_ps_vesitation.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
  COMMAND ${FXC} /nologo /T vs_5_0 /E VSMain /O3 /D PHASE_COMP=y /D BEND_COMP=z /Vn kWindPortVsGrass2
          /Fh ${SHADER_DIR}/wind_port_vs_grass2.h ${CMAKE_SOURCE_DIR}/src/wind_port.hlsl
  COMMAND ${FXC} /nologo /T vs_5_0 /E VSMain /O3 /D PHASE_COMP=x /D BEND_COMP=w /Vn kWindPortVsVesitation
          /Fh ${SHADER_DIR}/wind_port_vs_vesitation.h ${CMAKE_SOURCE_DIR}/src/wind_port.hlsl
  COMMAND ${FXC} /nologo /T vs_5_0 /E VSMain /O3 /D LOD1=1 /Vn kWindPortVsGrass2Lod1
          /Fh ${SHADER_DIR}/wind_port_vs_grass2_lod1.h ${CMAKE_SOURCE_DIR}/src/wind_port.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E PSMain /O3 /Vn kWindPortPs /Fh ${SHADER_DIR}/wind_port_ps.h
          ${CMAKE_SOURCE_DIR}/src/wind_port.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E PSMain /O3 /D ALPHA_TEST=1 /Vn kWindPortPsGrass2
          /Fh ${SHADER_DIR}/wind_port_ps_grass2.h ${CMAKE_SOURCE_DIR}/src/wind_port.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E PSMain /O3 /D ALPHA_TEST=2 /Vn kWindPortPsVesitation
          /Fh ${SHADER_DIR}/wind_port_ps_vesitation.h ${CMAKE_SOURCE_DIR}/src/wind_port.hlsl
  DEPENDS src/wind_port.hlsl
  VERBATIM)
add_custom_command(
  OUTPUT ${SHADER_DIR}/wind_merge_vs.h ${SHADER_DIR}/wind_merge_ps.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
  COMMAND ${FXC} /nologo /T vs_5_0 /E VSMain /O3 /Vn kWindMergeVs /Fh ${SHADER_DIR}/wind_merge_vs.h
          ${CMAKE_SOURCE_DIR}/src/wind_merge.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E PSMain /O3 /Vn kWindMergePs /Fh ${SHADER_DIR}/wind_merge_ps.h
          ${CMAKE_SOURCE_DIR}/src/wind_merge.hlsl
  DEPENDS src/wind_merge.hlsl
  VERBATIM)

add_library(EldenRingDLSS SHARED src/addon.cpp src/descriptors.cpp src/dlss.cpp src/wind.cpp src/wind_port.cpp
            ${SHADER_DIR}/vegetation_mask_cs.h ${SHADER_DIR}/wind_resolve_vs.h ${SHADER_DIR}/wind_resolve_ps.h
            ${SHADER_DIR}/wind_patch_cs.h ${SHADER_DIR}/wind_port_vs_grass2.h ${SHADER_DIR}/wind_port_vs_vesitation.h
            ${SHADER_DIR}/wind_port_vs_grass2_lod1.h ${SHADER_DIR}/wind_port_ps_grass2.h
            ${SHADER_DIR}/wind_port_ps_vesitation.h
            ${SHADER_DIR}/wind_port_ps.h ${SHADER_DIR}/wind_merge_vs.h ${SHADER_DIR}/wind_merge_ps.h)
target_include_directories(EldenRingDLSS PRIVATE src third_party/imgui ${SHADER_DIR})
target_include_directories(EldenRingDLSS SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
target_link_libraries(EldenRingDLSS PRIVATE reshade_utils ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib d3d12)
set_target_properties(EldenRingDLSS PROPERTIES SUFFIX ".addon64")
