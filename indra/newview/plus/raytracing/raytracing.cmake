# -*- cmake -*-
# Hardware ray tracing for photos, opt-in (research/230-hardware-raytracing.md in
# the sl-viewer repository). Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/raytracing/raytracing.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/raytracing/raytracing.h
    )
