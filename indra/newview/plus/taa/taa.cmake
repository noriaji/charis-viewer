# -*- cmake -*-
# Temporal anti-aliasing (research/185 in the sl-viewer repository): the last
# frame's picture brought to this frame by the camera's motion, after
# FXAA/SMAA, with the projection jittered by a fraction of a pixel. Included by
# plus/plus.cmake. Hooked in pipeline.cpp (renderFinalize, releaseScreenBuffers,
# rhiCoverageMainBegin) and llviewerdisplay.cpp (display: the jitter's scope);
# the pass is recorded by the Vulkan backend (llvulkan/llvktaa.cpp).

list(APPEND plus_SOURCE_FILES
    plus/taa/taa.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/taa/taa.h
    )
