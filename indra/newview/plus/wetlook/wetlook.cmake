# -*- cmake -*-
# Wet look: wet ground and puddles, made in the G-buffer before the lighting
# (research/120-rain-idea.md in the sl-viewer repository, stage 1). Uses the
# avatar bit of plus/skinsss's mark. Included by plus/plus.cmake. Drawn
# through the <plus> hook in LLPipeline::renderDeferredLighting()
# (pipeline.cpp); the passes are recorded by the Vulkan backend
# (llvulkan/llvkwetlook.cpp).

list(APPEND plus_SOURCE_FILES
    plus/wetlook/wetlook.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/wetlook/wetlook.h
    )
