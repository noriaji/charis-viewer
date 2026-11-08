# -*- cmake -*-
# The ocean (PlusOcean): surf foam and incoming waves on the sea, a shore field
# the water shader reads (research/112-ocean-shader-plan.md in the sl-viewer
# repository). Included by plus/plus.cmake. Read by the <plus> hook in
# lldrawpoolwater.cpp (renderPostDeferred) and class3/environment/waterF.glsl.

list(APPEND plus_SOURCE_FILES
    plus/ocean/ocean.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/ocean/ocean.h
    )
