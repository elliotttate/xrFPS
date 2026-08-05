cmake_minimum_required(VERSION 3.21)
project(openxr-xrfps VERSION 0.1.0 LANGUAGES CXX)

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

add_library(XrApiLayer_xrfps SHARED
    src/layer.cpp
    src/overlay.cpp
    src/hud.cpp
    src/config.cpp
)
target_include_directories(XrApiLayer_xrfps PRIVATE "${XRFPS_OPENXR_INCLUDE}")
target_link_libraries(XrApiLayer_xrfps PRIVATE d3d11 d3d12 dxgi gdiplus)
target_compile_definitions(XrApiLayer_xrfps PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

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
    foreach(XRFPS_THEME RANGE 1 5)
        add_test(NAME xrfps_hud_theme_${XRFPS_THEME}
            COMMAND xrfps_hudtest --theme ${XRFPS_THEME}
                    "${CMAKE_CURRENT_BINARY_DIR}/hudtest-theme-${XRFPS_THEME}.bmp")
    endforeach()
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
