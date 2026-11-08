# -*- cmake -*-
# The main view's sun shadow walks kept from frame to frame while nothing they
# read changed (LLRHI_SHADOW_SAME_INPUT, research/113-static-shadow-reuse.md in
# the sl-viewer repository). Included by plus/plus.cmake. Called from the
# <plus> hooks in pipeline.cpp (renderSunShadowCascades, shiftObjects).

list(APPEND plus_SOURCE_FILES
    plus/shadowreuse/shadowreuse.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shadowreuse/shadowreuse.h
    )
