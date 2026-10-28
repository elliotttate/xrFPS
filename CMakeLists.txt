cmake_minimum_required(VERSION 3.21)
project(openxr-xrfps VERSION 0.5.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the layer DLL has no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# OpenXR headers only — an API layer must NOT link the OpenXR loader.
# Pass -DOPENXR_SDK_PATH=<checkout> to use a local OpenXR-SDK; otherwise it is fetched.
if(DEFINED OPENXR_SDK_PATH)
    set(XRFPS_OPENXR_INCLUDE "${OPENXR_SDK_PATH}/include")
else()
    include(FetchContent)
    FetchContent_Declare(
        openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.49
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(openxr_sdk)
    if(NOT openxr_sdk_POPULATED)
        FetchContent_Populate(openxr_sdk)
    endif()
    set(XRFPS_OPENXR_INCLUDE "${openxr_sdk_SOURCE_DIR}/include")
endif()

if(NOT EXISTS "${XRFPS_OPENXR_INCLUDE}/openxr/openxr_loader_negotiation.h")
    message(FATAL_ERROR "openxr_loader_negotiation.h not found under ${XRFPS_OPENXR_INCLUDE} — need OpenXR SDK 1.0.33 or newer")
endif()

# SteamVR's compositor timing is the most authoritative desktop source for
# application FPS and CPU/GPU frametimes. Only the public header is consumed:
# the layer discovers and loads openvr_api.dll at runtime, so non-SteamVR
# systems gain no binary dependency.
include(FetchContent)
FetchContent_Declare(
    openvr_headers
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG v2.15.6
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(openvr_headers)
if(NOT openvr_headers_POPULATED)
    cmake_policy(PUSH)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(openvr_headers)
    cmake_policy(POP)
endif()
set(XRFPS_OPENVR_INCLUDE "${openvr_headers_SOURCE_DIR}/headers")

# Vulkan headers only, for timing Vulkan apps' GPU work: every entry point is
# loaded from the app's own vulkan-1.dll at run time.
FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.304
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(vulkan_headers)
if(NOT vulkan_headers_POPULATED)
    cmake_policy(PUSH)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(vulkan_headers)
    cmake_policy(POP)
endif()
set(XRFPS_VULKAN_INCLUDE "${vulkan_headers_SOURCE_DIR}/include")

add_library(XrApiLayer_xrfps SHARED
    src/layer.cpp
    src/overlay.cpp
    src/timing.cpp
    src/hud.cpp
    src/config.cpp
    src/frame_timing.cpp
    src/frame_timeline.h
    src/gpu_frame_timer.h
    src/vulkan_frame_timer.h
)
target_include_directories(XrApiLayer_xrfps PRIVATE
    "${XRFPS_OPENXR_INCLUDE}" "${XRFPS_OPENVR_INCLUDE}" "${XRFPS_VULKAN_INCLUDE}")
target_link_libraries(XrApiLayer_xrfps PRIVATE d3d11 d3d12 dxgi gdiplus opengl32)
target_compile_definitions(XrApiLayer_xrfps PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN XRFPS_VERSION="${PROJECT_VERSION}")

# The manifest travels next to the DLL; library_path is relative.
add_custom_command(TARGET XrApiLayer_xrfps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/manifest/XrApiLayer_xrfps.json"
        "$<TARGET_FILE_DIR:XrApiLayer_xrfps>/XrApiLayer_xrfps.json"
)

# Offline HUD renderer test: writes a BMP so the layout/font/graph can be
# inspected without a headset.
add_executable(xrfps_hudtest src/hud_test.cpp src/hud.cpp src/config.cpp)
target_include_directories(xrfps_hudtest PRIVATE "${XRFPS_OPENXR_INCLUDE}")
target_link_libraries(xrfps_hudtest PRIVATE gdiplus)
target_compile_definitions(xrfps_hudtest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME xrfps_hud_default
        COMMAND xrfps_hudtest "${CMAKE_CURRENT_BINARY_DIR}/hudtest-default.bmp")
    add_test(NAME xrfps_hud_dense
        COMMAND xrfps_hudtest --dense "${CMAKE_CURRENT_BINARY_DIR}/hudtest-dense.bmp")
    add_test(NAME xrfps_hud_separate_graphs
        COMMAND xrfps_hudtest --separate "${CMAKE_CURRENT_BINARY_DIR}/hudtest-separate.bmp")
    add_executable(xrfps_stats_test test/stats_test.cpp)
    add_test(NAME xrfps_stats COMMAND xrfps_stats_test)
    add_executable(xrfps_config_test test/config_test.cpp src/config.cpp)
    target_compile_definitions(xrfps_config_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME xrfps_config COMMAND xrfps_config_test)
    foreach(XRFPS_THEME RANGE 1 5)
        add_test(NAME xrfps_hud_theme_${XRFPS_THEME}
            COMMAND xrfps_hudtest --theme ${XRFPS_THEME}
                    "${CMAKE_CURRENT_BINARY_DIR}/hudtest-theme-${XRFPS_THEME}.bmp")
    endforeach()
    add_executable(xrfps_frame_timeline_test test/frame_timeline_tests.cpp)
    target_include_directories(xrfps_frame_timeline_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    add_test(NAME xrfps_frame_timeline COMMAND xrfps_frame_timeline_test)
    set_tests_properties(xrfps_frame_timeline PROPERTIES TIMEOUT 60)
    add_executable(xrfps_gpu_frame_timer_test test/gpu_frame_timer_tests.cpp)
    target_include_directories(xrfps_gpu_frame_timer_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(xrfps_gpu_frame_timer_test PRIVATE d3d12 d3d11 dxgi)
    add_test(NAME xrfps_gpu_frame_timer COMMAND xrfps_gpu_frame_timer_test)
    set_tests_properties(xrfps_gpu_frame_timer PROPERTIES TIMEOUT 60)

    # Manual runtime probe used by support/development. It is intentionally not
    # a CTest because SteamVR need not be installed on build machines.
    add_executable(xrfps_timing_probe test/timing_probe.cpp src/timing.cpp)
    target_include_directories(xrfps_timing_probe PRIVATE "${XRFPS_OPENVR_INCLUDE}")
    target_compile_definitions(xrfps_timing_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Native core shared by the Control Center: the real config, HUD renderer,
# installer and metrics collector behind a flat C ABI.
add_library(xrfps_core SHARED
    src/core_api.cpp
    src/config.cpp
    src/hud.cpp
    gui/installer.cpp
    gui/metrics_collector.cpp
)
target_include_directories(xrfps_core PRIVATE "${XRFPS_OPENXR_INCLUDE}")
target_link_libraries(xrfps_core PRIVATE gdiplus)
target_compile_definitions(xrfps_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# The app is .NET, which always uses the dynamic CRT.
set_property(TARGET xrfps_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Legacy ImGui Control Center, kept for a dependency-free fallback build.
option(XRFPS_BUILD_GUI "Build the ImGui Control Center" OFF)
if(XRFPS_BUILD_GUI)
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_executable(xrfps_control WIN32
        gui/main.cpp
        gui/installer.cpp
        gui/metrics_collector.cpp
        src/hud.cpp
        src/config.cpp
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    target_include_directories(xrfps_control PRIVATE
        "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends" "${XRFPS_OPENXR_INCLUDE}")
    target_link_libraries(xrfps_control PRIVATE d3d11 dxgi dwmapi shell32 gdiplus)
    target_compile_definitions(xrfps_control PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    # The Control Center installs the layer from its own directory.
    add_custom_command(TARGET xrfps_control POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:XrApiLayer_xrfps>" "$<TARGET_FILE_DIR:xrfps_control>/"
    )
    add_dependencies(xrfps_control XrApiLayer_xrfps)
endif()

# Minimal OpenXR D3D11 app for exercising the layer end-to-end (needs the
# OpenXR loader, built from the SDK sources).
option(XRFPS_BUILD_TESTAPP "Build the minimal OpenXR test app" ON)
if(XRFPS_BUILD_TESTAPP)
    if(DEFINED OPENXR_SDK_PATH)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
        set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
        set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
        add_subdirectory("${OPENXR_SDK_PATH}" openxr_sdk_build EXCLUDE_FROM_ALL)
    endif()
    if(TARGET openxr_loader)
        add_executable(xrfps_testapp test/xr_test_app.cpp)
        target_include_directories(xrfps_testapp PRIVATE "${XRFPS_OPENXR_INCLUDE}")
        target_link_libraries(xrfps_testapp PRIVATE openxr_loader d3d11 d3d12 dxgi)
        target_compile_definitions(xrfps_testapp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        # The SDK loader builds with the dynamic CRT; match it here (the layer
        # itself stays static-CRT).
        set_property(TARGET xrfps_testapp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    else()
        message(STATUS "openxr_loader target unavailable; skipping xrfps_testapp")
    endif()
endif()
