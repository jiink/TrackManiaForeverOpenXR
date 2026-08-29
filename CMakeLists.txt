cmake_minimum_required(VERSION 3.24)

project(TrackManiaOpenXRBridge LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This project builds a Win32 Direct3D 9 proxy and requires MSVC on Windows.")
endif()

include(FetchContent)
FetchContent_Declare(
  openxr_headers
  URL https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/refs/tags/release-1.1.62.zip
  URL_HASH SHA256=FBEFD5E39E7D0D4C7D4552FDAB30A65AEB907D84B6C17AD5A06F7CD3F66CAAA0
)
FetchContent_MakeAvailable(openxr_headers)

add_library(d3d9 SHARED
  src/d3d9_proxy.cpp
  src/log.cpp
  src/vr_bridge.cpp
  src/d3d9_proxy.def
)

target_compile_features(d3d9 PRIVATE cxx_std_20)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_NO_PROTOTYPES)
target_include_directories(d3d9 PRIVATE
  "${openxr_headers_SOURCE_DIR}/src"
  "${openxr_headers_BINARY_DIR}/include"
)
target_link_libraries(d3d9 PRIVATE dxguid d3d11 dxgi)

add_executable(d3d9on12_compat_smoke EXCLUDE_FROM_ALL tests/d3d9on12_compat_smoke.cpp)
target_compile_features(d3d9on12_compat_smoke PRIVATE cxx_std_20)
target_compile_definitions(d3d9on12_compat_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d9on12_compat_smoke PRIVATE dxguid d3d11 d3d12)

set_target_properties(d3d9 PROPERTIES
  OUTPUT_NAME "d3d9"
  PREFIX ""
  SUFFIX ".dll"
)
