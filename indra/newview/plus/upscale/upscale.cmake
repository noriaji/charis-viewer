# -*- cmake -*-
# Upscaling: the main view drawn smaller and brought to the window's size
# with AMD FidelityFX Super Resolution 1 (EASU, RCAS), research/165 in the
# sl-viewer repository. Included by plus/plus.cmake. Hooked in pipeline.cpp
# (allocateScreenBufferInternal, resizeScreenTexture, releaseScreenBuffers,
# applyFXAA, renderFinalize) and llviewerwindow.cpp (the snapshots' size to
# restore); the passes are recorded by the Vulkan backend
# (llvulkan/llvkupscale.cpp).

list(APPEND plus_SOURCE_FILES
    plus/upscale/upscale.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/upscale/upscale.h
    )
