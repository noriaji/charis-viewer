# -*- cmake -*-
# The sun shadow cascades' spatial bridge culls: each bridge's octree walked
# once per cascade set instead of once per cascade (LLRHI_SHADOW_BRIDGE_ONCE,
# research/178 section 9 in the sl-viewer repository). Included by
# plus/plus.cmake. Called from the <plus> hooks in pipeline.cpp
# (stateSortDrawableGroups, renderSunShadowCascades).

list(APPEND plus_SOURCE_FILES
    plus/shadowbridge/shadowbridge.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shadowbridge/shadowbridge.h
    )
