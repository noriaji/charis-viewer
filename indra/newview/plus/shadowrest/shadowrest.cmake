# -*- cmake -*-
# The shadow casters still drawn through the GL translation, handed to the
# native shadow packets instead (research/137 N10 in the sl-viewer repository):
# the Linden trees first. Included by plus/plus.cmake. Reached through the
# "LLRHI native (research/137 N10)" lines in pipeline.cpp
# (renderShadowCasters, drawNativeShadowCasters) and lldrawpooltree.cpp
# (LLDrawPoolTree::renderShadow).

list(APPEND plus_SOURCE_FILES
    plus/shadowrest/shadowrest.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shadowrest/shadowrest.h
    )
