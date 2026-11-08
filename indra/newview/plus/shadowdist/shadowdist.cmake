# -*- cmake -*-
# The shadow distance (PlusShadowDistance): how far the sun's shadows reach and
# how sharp they stay there, part of the graphics presets and the snapshot boost
# (research/107-shadow-distance.md in the sl-viewer repository). Included by
# plus/plus.cmake. Read by the <plus> hooks in pipeline.cpp (generateSunShadow,
# allocateShadowBuffer, bindDeferredShader).

list(APPEND plus_SOURCE_FILES
    plus/shadowdist/shadowdist.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shadowdist/shadowdist.h
    )
