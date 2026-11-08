# -*- cmake -*-
# The idle objects segment cut into its steps (LLRHI_IDLE_CENSUS=1) and the
# avatars' default parameter check from a kept list (LLRHI_VPARAM_MEMO)
# (research/193 in the sl-viewer repository). Included by plus/plus.cmake.
# Hooks: llappviewer.cpp idle(), LLViewerObjectList::update,
# LLViewerObject::idleUpdate, LLVOVolume::animateTextures, LLVOAvatar
# (idle steps, visualParamWeightsAreDefault), LLPipeline::updateMove*.

list(APPEND plus_SOURCE_FILES
    plus/idlecheap/idlecensus.cpp
    plus/idlecheap/vparammemo.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/idlecheap/idlecensus.h
    plus/idlecheap/vparammemo.h
    )
