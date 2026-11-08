# -*- cmake -*-
# Volumetric light: sun shafts, the light the air scatters towards the eye
# past the shadows (research/84-volumetric-lighting.md in the sl-viewer
# repository). Included by plus/plus.cmake. Drawn through the <plus> hook in
# LLPipeline::renderFinalize() (pipeline.cpp); the passes are recorded by the
# Vulkan backend (llvulkan/llvkvolumetric.cpp).

list(APPEND plus_SOURCE_FILES
    plus/volumetric/volumetric.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/volumetric/volumetric.h
    )
