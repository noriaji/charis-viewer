# -*- cmake -*-
# Alpha draw lists (research/137 N3, LLRHI_DRAW_LISTS_ALPHA): the alpha pool's
# batch loop (LLDrawPoolAlpha::renderAlpha) hands its draws to the backend as
# draw lists (llrhi/llrhidrawlist.h) instead of one direct draw each. Included
# by plus/plus.cmake. Called by the "LLRHI native (research/137 N3)" line in
# lldrawpoolalpha.cpp.

list(APPEND plus_SOURCE_FILES
    plus/alphalist/alphalist.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/alphalist/alphalist.h
    )
