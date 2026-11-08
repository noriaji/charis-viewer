# -*- cmake -*-
# The sun shadow cascades leave out small moving casters far from the camera
# (PlusShadowSkipFarSmall, research/178 C2 in the sl-viewer repository).
# Included by plus/plus.cmake. Called from the <plus> hooks in pipeline.cpp
# (renderSunShadowCascades' sort, both ways: the chunks and the loop).

list(APPEND plus_SOURCE_FILES
    plus/shadowfar/shadowfar.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shadowfar/shadowfar.h
    )
