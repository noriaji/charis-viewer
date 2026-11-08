# -*- cmake -*-
# Stage 0 of the retained, GPU driven main view: the census of the main
# camera's static opaque batches per group and render type (LLRHI_RETAINED_CENSUS,
# research/195-vulkan-native-architecture.md 3.1 in the sl-viewer repository).
# Included by plus/plus.cmake. Called from the <plus> line at the end of
# LLPipeline::postSort's render map (pipeline.cpp) and counted from <plus>
# lines in llspatialpartition.cpp (LLDrawInfo made and freed, rebuildGeom).

list(APPEND plus_SOURCE_FILES
    plus/retained/retained.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/retained/retained.h
    )
