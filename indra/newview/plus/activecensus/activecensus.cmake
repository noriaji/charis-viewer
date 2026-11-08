# -*- cmake -*-
# Why drawables became active (the census, LLRHI_FRAME_COST=2), and the
# orphan reunion that leaves a still static parent static (LLRHI_ACTIVE_ORPHAN=1),
# research/180 10.12 in the sl-viewer repository.
# Included by plus/plus.cmake. Hooks: LLDrawable::makeActive,
# LLViewerObjectList::findOrphans, LLVOVolume::onDrawableUpdateFromServer,
# LLPipeline::updateMove.

list(APPEND plus_SOURCE_FILES
    plus/activecensus/activecensus.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/activecensus/activecensus.h
    )
