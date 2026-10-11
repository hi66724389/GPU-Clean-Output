cmake_minimum_required(VERSION 3.16...3.28)

project(gpu-clean-output VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce MSVC flags
if(MSVC)
    add_compile_options(/EHsc /permissive- /W4 /utf-8)
    add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
endif()


# Try standard find_package first
find_package(libobs QUIET)

add_library(gpu-clean-output MODULE
    src/plugin-main.cpp
    src/clean-output-output.hpp
    src/clean-output-output.cpp
)

if(WIN32)
    target_sources(gpu-clean-output PRIVATE
        src/special-display-manager.hpp
        src/special-display-manager.cpp
        src/d3d11-output-renderer.hpp
        src/d3d11-output-renderer.cpp
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)

    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(DRM_LEASE_XML "${WAYLAND_PROTOCOLS_PREFIX}/share/wayland-protocols/staging/drm-lease/drm-lease-v1.xml")
    if(NOT EXISTS "${DRM_LEASE_XML}")
        execute_process(
            COMMAND pkg-config --variable=pkgdatadir wayland-protocols
            OUTPUT_VARIABLE WAYLAND_PROTOCOLS_DATADIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(DRM_LEASE_XML "${WAYLAND_PROTOCOLS_DATADIR}/staging/drm-lease/drm-lease-v1.xml")
    endif()
    if(NOT EXISTS "${DRM_LEASE_XML}")
        message(FATAL_ERROR "Could not find staging/drm-lease/drm-lease-v1.xml")
    endif()

    set(DRM_LEASE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/drm-lease")
    file(MAKE_DIRECTORY "${DRM_LEASE_GEN_DIR}")
    add_custom_command(
        OUTPUT "${DRM_LEASE_GEN_DIR}/drm-lease-v1-client-protocol.h"
               "${DRM_LEASE_GEN_DIR}/drm-lease-v1-protocol.c"
        COMMAND "${WAYLAND_SCANNER}" client-header "${DRM_LEASE_XML}"
                "${DRM_LEASE_GEN_DIR}/drm-lease-v1-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${DRM_LEASE_XML}"
                "${DRM_LEASE_GEN_DIR}/drm-lease-v1-protocol.c"
        DEPENDS "${DRM_LEASE_XML}"
    )
    target_sources(gpu-clean-output PRIVATE
        src/special-display-manager.hpp
        src/special-display-manager.cpp
        src/d3d11-output-renderer.hpp
        src/d3d11-output-renderer.cpp
        "${DRM_LEASE_GEN_DIR}/drm-lease-v1-protocol.c"
        "${DRM_LEASE_GEN_DIR}/drm-lease-v1-client-protocol.h"
    )
    target_include_directories(gpu-clean-output PRIVATE "${DRM_LEASE_GEN_DIR}")
    target_link_libraries(gpu-clean-output PRIVATE
        PkgConfig::WAYLAND
        PkgConfig::LIBDRM
    )
endif()

target_include_directories(gpu-clean-output PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(TARGET OBS::libobs)
    target_link_libraries(gpu-clean-output PRIVATE OBS::libobs)
else()
    set(OBS_SDK_DIR "E:/code/drop/DistroAV-GPU/.deps/obs-studio-31.1.1" CACHE PATH "Path to OBS Studio source/deps")
    
    target_include_directories(gpu-clean-output PRIVATE
        ${OBS_SDK_DIR}/libobs
        ${OBS_SDK_DIR}/frontend/api
        ${OBS_SDK_DIR}/build
        ${OBS_SDK_DIR}/build_x64/config
        ${OBS_SDK_DIR}/build_x64/libobs
        /app/include
    )
    
    find_library(LIBOBS_LIB
        NAMES obs libobs
        PATHS
            ${OBS_SDK_DIR}/build_x64/libobs/Release
            ${OBS_SDK_DIR}/build_x64/libobs/Debug
            ${OBS_SDK_DIR}/build/libobs
            /app/lib
            /app/lib/obs
            "C:/Program Files/obs-studio/bin/64bit"
    )

    find_library(OBS_FRONTEND_LIB
        NAMES obs-frontend-api
        PATHS
            ${OBS_SDK_DIR}/build_x64/frontend/api/Release
            ${OBS_SDK_DIR}/build_x64/frontend/api/Debug
            ${OBS_SDK_DIR}/build/frontend/api
            /app/lib
            /app/lib/obs
            "C:/Program Files/obs-studio/bin/64bit"
    )
    
    target_link_libraries(gpu-clean-output PRIVATE ${LIBOBS_LIB})
    if(OBS_FRONTEND_LIB)
        target_link_libraries(gpu-clean-output PRIVATE ${OBS_FRONTEND_LIB})
    endif()
endif()

if(WIN32)
    target_link_libraries(gpu-clean-output PRIVATE
        d3d11.lib
        dxgi.lib
        dxguid.lib
        windowsapp.lib
    )
endif()

set_target_properties(gpu-clean-output PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gpu-clean-output"
)

include(GNUInstallDirs)

install(TARGETS gpu-clean-output
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
)
