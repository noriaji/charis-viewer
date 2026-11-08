# -*- cmake -*-
# Screen space indirect light (SSGI): GTAO's occlusion widened into a
# visibility bitmask that also gathers the light near surfaces send back
# (research/164-ssgi.md in the sl-viewer repository). Included by
# plus/plus.cmake. Drawn from plus/gtao and the <plus> hooks in
# LLPipeline::renderDeferredLighting() and bindReflectionProbes()
# (pipeline.cpp); the passes are recorded by the Vulkan backend
# (llvulkan/llvkssgi.cpp); softenLightF.glsl adds the light.

list(APPEND plus_SOURCE_FILES
    plus/ssgi/ssgi.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/ssgi/ssgi.h
    )
