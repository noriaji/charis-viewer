# -*- cmake -*-
# After a teleport, the old region's textures go down without waiting for the
# simulator to drop the region (research/97-tp-release.md in the sl-viewer
# repository). Included by plus/plus.cmake. Hooks: two calls in
# llviewertexturelist.cpp.

list(APPEND plus_SOURCE_FILES
    plus/tprelease/tprelease.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/tprelease/tprelease.h
    )
