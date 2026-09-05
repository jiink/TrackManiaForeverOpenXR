cmake_minimum_required(VERSION 3.24)

project(TrackManiaForeverOpenXR LANGUAGES CXX)

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

FetchContent_Declare(
  dear_imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
  URL_HASH SHA256=E1C46D676C2BCB7CED847BA27F50553E33A19DB97B3CADAEC7F8BE64449139F8
)
FetchContent_MakeAvailable(dear_imgui)

add_library(d3d9 SHARED
  src/d3d9_proxy.cpp
  src/log.cpp
  src/runtime_paths.cpp
  src/vr_bridge.cpp
  src/d3d9_proxy.def
  ${dear_imgui_SOURCE_DIR}/imgui.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_draw.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_tables.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_compile_features(d3d9 PRIVATE cxx_std_20)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_NO_PROTOTYPES)
target_include_directories(d3d9 PRIVATE
  "${openxr_headers_SOURCE_DIR}/src"
  "${openxr_headers_BINARY_DIR}/include"
  "${dear_imgui_SOURCE_DIR}"
  "${dear_imgui_SOURCE_DIR}/backends"
)
target_link_libraries(d3d9 PRIVATE dxguid d3d11 dxgi)

add_executable(d3d9on12_compat_smoke EXCLUDE_FROM_ALL tests/d3d9on12_compat_smoke.cpp)
target_compile_features(d3d9on12_compat_smoke PRIVATE cxx_std_20)
target_compile_definitions(d3d9on12_compat_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d9on12_compat_smoke PRIVATE dxguid d3d11 d3d12)

add_executable(tmloader_iat_smoke EXCLUDE_FROM_ALL tests/tmloader_iat_smoke.cpp)
target_compile_features(tmloader_iat_smoke PRIVATE cxx_std_20)
target_compile_definitions(tmloader_iat_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(tmloader_iat_smoke PROPERTIES OUTPUT_NAME "TmForever")

set_target_properties(d3d9 PROPERTIES
  OUTPUT_NAME "d3d9"
  ARCHIVE_OUTPUT_NAME "TMFOXR"
  PREFIX ""
  SUFFIX ".dll"
)

add_custom_command(TARGET d3d9 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:d3d9>
    $<TARGET_FILE_DIR:d3d9>/TMFOXR.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${dear_imgui_SOURCE_DIR}/LICENSE.txt
    $<TARGET_FILE_DIR:d3d9>/DearImGui-LICENSE.txt
  COMMENT "Creating the TrackMania ModLoader TMFOXR.dll"
)
