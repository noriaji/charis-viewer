# -*- cmake -*-
# GTAO and contact shadows: ambient occlusion and short sun shadows written
# into the deferred light map (research/92-gtao.md in the sl-viewer
# repository). Included by plus/plus.cmake. Drawn through the <plus> hook in
# LLPipeline::renderDeferredLighting() (pipeline.cpp); the passes are recorded
# by the Vulkan backend (llvulkan/llvkgtao.cpp).

list(APPEND plus_SOURCE_FILES
    plus/gtao/gtao.cpp
    plus/gtao/gtao_ui.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/gtao/gtao.h
    plus/gtao/gtao_ui.h
    )
