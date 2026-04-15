cmake_minimum_required(VERSION 3.16)
project(PopTB_World CXX)

# PopTB_World — a modern, clean-room implementation of a 1998-era planetary
# worldview renderer (software, 8bpp palette-indexed, atmospheric halo, LOD
# terrain mesh, mouse panning, selection rectangle). No external binary
# dependencies; produces one static library + one Win32 demo viewer.
#
# Layout:
#   src/      — the library (namespace ptw).
#   app/      — the Win32 viewer (WinMain) and data-file loaders.
#   demo/     — data files consumed by the viewer at runtime.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W3 /D_CRT_SECURE_NO_WARNINGS /wd4244 /wd4267)
else()
    add_compile_options(-Wall -Wno-narrowing)
endif()

add_library(PopTBWorld STATIC
    src/raster.cpp
    src/atmo.cpp
    src/lod.cpp
    src/worldview.cpp
    src/level.cpp
)

target_include_directories(PopTBWorld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- Viewer app ----------------------------------------------------------

option(PTW_BUILD_VIEWER "Build the Win32 viewer (World.exe)" ON)
if(PTW_BUILD_VIEWER)
    add_executable(World WIN32
        app/viewer.cpp
        app/datafiles.cpp
    )
    target_link_libraries(World PRIVATE PopTBWorld gdi32 user32)
    set_target_properties(World PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_SOURCE_DIR}/build_output"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/build_output"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/build_output"
    )
endif()
