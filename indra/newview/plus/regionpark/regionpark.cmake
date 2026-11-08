# -*- cmake -*-
# Teleport and revisit speed: the textures of the regions just left stay while
# VRAM allows, and after an arrival the cached objects in reach are made before
# they come into view (research/242-region-park.md in the sl-viewer
# repository). Included by plus/plus.cmake. Hooks: the top of two functions of
# plus/tprelease, one block in LLViewerRegion::idleUpdate().

list(APPEND plus_SOURCE_FILES
    plus/regionpark/regionpark.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/regionpark/regionpark.h
    )
