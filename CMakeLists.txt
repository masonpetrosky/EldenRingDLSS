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

add_library(EldenRingDLSS SHARED src/addon.cpp src/descriptors.cpp src/dlss.cpp)
target_include_directories(EldenRingDLSS PRIVATE src third_party/imgui)
target_include_directories(EldenRingDLSS SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
target_link_libraries(EldenRingDLSS PRIVATE reshade_utils ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib d3d12)
set_target_properties(EldenRingDLSS PROPERTIES SUFFIX ".addon64")
