# -*- cmake -*-
# The alpha pool's glow pass without the batches whose faces have no glow,
# and the alpha pass's CPU counters (research/178 F0 in the sl-viewer
# repository). Included by plus/plus.cmake. Hooks: LLDrawPoolAlpha::renderAlpha
# (and plus/alphalist's copy), LLVolumeGeometryManager::registerFace and
# rebuildMesh, LLDrawInfo::mRHIGlow.

list(APPEND plus_SOURCE_FILES
    plus/alphapass/alphapass.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/alphapass/alphapass.h
    )
