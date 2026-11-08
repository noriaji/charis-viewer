# -*- cmake -*-
# Material lists (research/137 N4 in the sl-viewer repository): the legacy
# material pool's and the bump pool's batch loops handed to the backend as one
# draw list (LLRHI::DrawList, llrhidrawlist.h) instead of one direct draw and
# its uniform and texture calls per batch. Off by default (LLRHI_DRAW_LISTS and
# LLRHI_DRAW_LISTS_MATERIAL). Included by plus/plus.cmake. Called from the
# "LLRHI native (research/137 N4)" lines in lldrawpoolmaterials.cpp and
# lldrawpoolbump.cpp.

list(APPEND plus_SOURCE_FILES
    plus/materiallist/materiallist.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/materiallist/materiallist.h
    )
