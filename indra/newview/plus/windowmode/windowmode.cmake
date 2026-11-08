# -*- cmake -*-
# Borderless fullscreen (plus/windowmode): the window loses its frame and fills its monitor, and goes
# back, while the viewer runs (no restart; no change of the display mode). Included by plus/plus.cmake.
# The window part is LLWindowWin32::setBorderless (llwindow); here are the setting and its switch.

list(APPEND plus_SOURCE_FILES
    plus/windowmode/windowmode.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/windowmode/windowmode.h
    )
