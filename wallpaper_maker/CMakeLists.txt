add_executable(wallpaper_maker
    maker_main.cpp
    ffmpeg_runner.cpp
    ffmpeg_runner.h
    h264_processor.cpp
    h264_processor.h
    sps_parser.cpp
    sps_parser.h
)

target_link_libraries(wallpaper_maker PRIVATE lwp_common)

target_compile_definitions(wallpaper_maker PRIVATE _CRT_SECURE_NO_WARNINGS)

target_include_directories(wallpaper_maker PRIVATE
    ${CMAKE_SOURCE_DIR}/common
)

# Maker is a console tool; does NOT link FFmpeg (calls ffmpeg.exe at runtime).
set_target_properties(wallpaper_maker PROPERTIES
    WIN32_EXECUTABLE OFF
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)
