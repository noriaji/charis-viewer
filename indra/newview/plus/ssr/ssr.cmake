# -*- cmake -*-
# Screen space reflections in passes of their own: a port of Linden Lab's
# project/graphics-care-package (Hi-Z tracing; research/129-gcp-ssr-port.md in
# the sl-viewer repository). Included by plus/plus.cmake. Drawn through the
# <plus> hooks in LLPipeline::renderDeferredLighting() and
# bindReflectionProbes() (pipeline.cpp) and read by reflectionProbeF.glsl
# (PLUS_SSR, defined in llviewershadermgr.cpp); the passes are recorded by the
# Vulkan backend (llvulkan/llvkssr.cpp).

list(APPEND plus_SOURCE_FILES
    plus/ssr/ssr.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/ssr/ssr.h
    )
