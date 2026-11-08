# -*- cmake -*-
# Skin once, part (a): an avatar's skinning palette asked of the backend once
# per build instead of once per pass (LLRHI_SKIN_ONCE / PlusSkinOnce,
# research/133-skin-once.md in the sl-viewer repository). Included by
# plus/plus.cmake. Called from the <plus> hook in LLRenderPass::rhiPaletteRow()
# (lldrawpool.cpp).

list(APPEND plus_SOURCE_FILES
    plus/skinonce/skinonce.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/skinonce/skinonce.h
    )
