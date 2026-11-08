# -*- cmake -*-
# Atmosphere effects for photography: ground fog, light shafts in the mist and
# fireflies (research/148-atmosphere-effects.md in the sl-viewer repository).
# Included by plus/plus.cmake. Drawn through the <plus> hooks in
# LLDrawPoolAlpha::renderPostDeferred() (the fog, before the alpha) and
# LLPipeline::renderFinalize() (pipeline.cpp); the passes are recorded by the
# Vulkan backend (llvulkan/llvkatmosphere.cpp).

list(APPEND plus_SOURCE_FILES
    plus/atmosphere/atmosphere.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/atmosphere/atmosphere.h
    )
