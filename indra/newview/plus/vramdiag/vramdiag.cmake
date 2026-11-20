# -*- cmake -*-
# A census of the images over 8 MB (the textures of the list: what they are, BC7 or plain, how far down) and the VRAM
# state around it, to find out what an overrun of the driver's VRAM budget is made of (LLRHI_VRAM_DIAG=1, research/354
# vram-stall-diag in the sl-viewer repository). Included by plus/plus.cmake. The device's half (what was made while over
# the budget, the heaps) is indra/llvulkan/llvkvramdiag.cpp.
# Hook: LLViewerTexture::updateClass (once a frame, at its end).

list(APPEND plus_SOURCE_FILES
    plus/vramdiag/vramdiag.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/vramdiag/vramdiag.h
    )
