# -*- cmake -*-
# Draw lists for the draw pools (research/137 N2 in the sl-viewer repository,
# LLRHI_DRAW_LISTS with LLRHI_DRAW_LISTS_OPAQUE): LLRenderPass's batch loops
# hand their batches to the backend as one list of values instead of one
# direct draw each. drawlistbatch.* is the shared "batch -> Draw" part the
# alpha (N3) and material (N4) lists use too. Included by plus/plus.cmake.
# Called from the hooks marked "LLRHI native (research/137 N2)" in
# lldrawpool.cpp.

list(APPEND plus_SOURCE_FILES
    plus/drawlist/drawlist.cpp
    plus/drawlist/drawlistbatch.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/drawlist/drawlist.h
    plus/drawlist/drawlistbatch.h
    )
