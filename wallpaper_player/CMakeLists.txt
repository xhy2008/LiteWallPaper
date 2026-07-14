add_executable(wallpaper_player WIN32
    player_main.cpp
    app.h
    app.cpp
    nal_parser.h
    nal_parser.cpp
    d3d11_decoder.h
    d3d11_decoder.cpp
    d3d11_renderer.h
    d3d11_renderer.cpp
    wallpaper_window.h
    wallpaper_window.cpp
    tray_icon.h
    tray_icon.cpp
    config_manager.h
    config_manager.cpp
    foreground_monitor.h
    foreground_monitor.cpp
    resource.rc
)

target_link_libraries(wallpaper_player PRIVATE lwp_common)

target_include_directories(wallpaper_player PRIVATE
    ${CMAKE_SOURCE_DIR}/common
)

# Windows GUI subsystem, lean binary.
set_target_properties(wallpaper_player PROPERTIES
    OUTPUT_NAME "LiteWallPaper"
    WIN32_EXECUTABLE ON
    LINK_FLAGS "/MANIFEST:NO"
)

# The manifest is embedded via resource.rc (1 24 "app.manifest"). The linker's
# own /MANIFEST step is disabled above to avoid a duplicate RT_MANIFEST
# resource (CVT1100).
