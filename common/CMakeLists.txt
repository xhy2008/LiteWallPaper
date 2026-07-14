# Header-only common module shared by maker and player.
# Defines the .lwp container format (magic header + raw H.264 Annex B payload).
add_library(lwp_common INTERFACE)
target_include_directories(lwp_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Windows graphics APIs (linked at the OS level; no extra deps).
target_link_libraries(lwp_common INTERFACE
    d3d11 dxgi dxguid d3dcompiler dwmapi user32 shell32 gdi32
    shlwapi comctl32 comdlg32)
