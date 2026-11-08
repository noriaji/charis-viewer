# -*- cmake -*-
# Softer shadows on faces: a fill light and weaker ambient occlusion on the
# skin only (research/110-skin-fill.md in the sl-viewer repository). Uses the
# skin mark of plus/skinsss. Included by plus/plus.cmake. Drawn through the
# <plus> hooks in LLPipeline::renderDeferredLighting() (pipeline.cpp); the
# passes are recorded by the Vulkan backend (llvulkan/llvkskinfill.cpp).

list(APPEND plus_SOURCE_FILES
    plus/skinfill/skinfill.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/skinfill/skinfill.h
    )
