cmake_minimum_required(VERSION 3.20)

project(LiteWallPaper
    VERSION 1.0.0
    DESCRIPTION "Extremely lightweight Windows dynamic wallpaper"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CRT for single-file distribution.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /SUBSYSTEM:WINDOWS")

if(MSVC)
    add_compile_options(/utf-8 /EHsc /W3 /permissive-)
    add_compile_options($<$<CONFIG:Release>:/O2>)
    add_compile_options($<$<CONFIG:Release>:/Gy>)
    add_compile_options($<$<CONFIG:Release>:/Gw>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF>)
    add_link_options($<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Neither the maker nor the player links FFmpeg. The maker calls ffmpeg.exe
# at runtime (only needs it on PATH); the player has zero media deps.
add_subdirectory(common)
add_subdirectory(wallpaper_maker)
add_subdirectory(wallpaper_player)
