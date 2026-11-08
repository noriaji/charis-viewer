# -*- cmake -*-
# Hair sway: the agent's rigged hair lags behind the head (research/hair-sway.md
# in the sl-viewer repository). Included by plus/plus.cmake. Drawn through the
# <plus> hook in LLVOAvatar::updateSkinInfoMatrixPalette() (llvoavatar.cpp).

list(APPEND plus_SOURCE_FILES
    plus/hairsway/hairdetect.cpp
    plus/hairsway/hairsway.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/hairsway/hairdetect.h
    plus/hairsway/hairsway.h
    )
