# -*- cmake -*-
# Glare (HDR bloom) and the sun's lens flare, added to the scene before tone
# mapping (research/108-glare.md in the sl-viewer repository). Included by
# plus/plus.cmake. Drawn through the <plus> hook in LLPipeline::renderFinalize()
# (pipeline.cpp, after generateExposure); the passes are recorded by the
# Vulkan backend (llvulkan/llvkglare.cpp).

list(APPEND plus_SOURCE_FILES
    plus/glare/glare.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/glare/glare.h
    )
