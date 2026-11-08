# -*- cmake -*-
# The main camera's octree walks on the cull threads, by subtree below each
# partition's root (LLRHI_CULL_PAR, research/184-multithread-render.md A1 in
# the sl-viewer repository), and replayed from their last trace while the
# camera and the octree stay the same (LLRHI_CULL_REUSE, research/188).
# Included by plus/plus.cmake. Called from the <plus> hooks in pipeline.cpp
# (updateCull, shiftObjects, the cull threads' constructor); the walk code
# records through cullrecord.h (pipeline.cpp, llspatialpartition.cpp,
# llvieweroctree.cpp) and is traced in llspatialpartition.cpp (cullTraced).

list(APPEND plus_SOURCE_FILES
    plus/cullinto/cullinto.cpp
    plus/cullinto/cullreuse.cpp
    plus/cullinto/cullverify.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/cullinto/cullinto.h
    plus/cullinto/cullrecord.h
    plus/cullinto/cullreuse.h
    plus/cullinto/cullverify.h
    )
