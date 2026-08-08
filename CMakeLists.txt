cmake_minimum_required(VERSION 3.24)

project(TrackManiaOpenXRBridge LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This project builds a Win32 Direct3D 9 proxy and requires MSVC on Windows.")
endif()

include(FetchContent)
FetchContent_Declare(
  openxr_headers
  URL https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/refs/tags/release-1.1.52.zip
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

set_target_properties(d3d9 PROPERTIES
  OUTPUT_NAME "d3d9"
  PREFIX ""
  SUFFIX ".dll"
)
