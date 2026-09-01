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
    src/special-display-manager.hpp
    src/special-display-manager.cpp
    src/d3d11-output-renderer.hpp
    src/d3d11-output-renderer.cpp
    src/clean-output-output.hpp
    src/clean-output-output.cpp
)

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
        ${OBS_SDK_DIR}/build_x64/config
        ${OBS_SDK_DIR}/build_x64/libobs
    )
    
    find_library(LIBOBS_LIB
        NAMES obs libobs
        PATHS
            ${OBS_SDK_DIR}/build_x64/libobs/Release
            ${OBS_SDK_DIR}/build_x64/libobs/Debug
            "C:/Program Files/obs-studio/bin/64bit"
    )

    find_library(OBS_FRONTEND_LIB
        NAMES obs-frontend-api
        PATHS
            ${OBS_SDK_DIR}/build_x64/frontend/api/Release
            ${OBS_SDK_DIR}/build_x64/frontend/api/Debug
            "C:/Program Files/obs-studio/bin/64bit"
    )
    
    target_link_libraries(gpu-clean-output PRIVATE ${LIBOBS_LIB})
    if(OBS_FRONTEND_LIB)
        target_link_libraries(gpu-clean-output PRIVATE ${OBS_FRONTEND_LIB})
    endif()
endif()

target_link_libraries(gpu-clean-output PRIVATE
    d3d11.lib
    dxgi.lib
    dxguid.lib
    windowsapp.lib
)

set_target_properties(gpu-clean-output PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gpu-clean-output"
)
