# -*- cmake -*-
# Draws the ranges text chat carries (whisper, say, shout) on the minimap. Included by plus/plus.cmake.
# Hook: one call in LLNetMap::draw() (llnetmap.cpp).

list(APPEND plus_SOURCE_FILES
    plus/minimapchat/minimapchat.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/minimapchat/minimapchat.h
    )
