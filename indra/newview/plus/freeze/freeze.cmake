# -*- cmake -*-
# World freeze: stop the world's clock for photography while the camera, the
# UI, the settings and the detail/texture loading keep running
# (world-freeze, 2026-10-17). Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/freeze/freeze.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/freeze/freeze.h
    )
