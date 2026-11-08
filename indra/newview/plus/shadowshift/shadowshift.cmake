# -*- cmake -*-
# Moving sun shadows (PlusShadowShift): a cached cascade moved by whole texels
# instead of drawn again whole (research/151-shadow-shift-reuse.md in the
# sl-viewer repository). Included by plus/plus.cmake. Called by the <plus>
# hooks in pipeline.cpp (reuseShadowCascade, renderCachedShadowCascade,
# releaseShadowCaches).

list(APPEND plus_SOURCE_FILES
    plus/shadowshift/shadowshift.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shadowshift/shadowshift.h
    )
