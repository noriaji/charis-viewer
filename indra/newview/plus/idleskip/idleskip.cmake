# -*- cmake -*-
# Texture animations and spins (llTargetOmega) of objects nobody sees are not
# stepped every frame (research/178 D1, D2 in the sl-viewer repository).
# Included by plus/plus.cmake. Hooks: LLVOVolume::animateTextures,
# LLViewerObject::idleUpdate, LLViewerObjectList::update.

list(APPEND plus_SOURCE_FILES
    plus/idleskip/idleskip.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/idleskip/idleskip.h
    )
