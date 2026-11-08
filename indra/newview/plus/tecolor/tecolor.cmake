# -*- cmake -*-
# TE color without a group rebuild: a face's color (and its alpha while it stays
# in the same transparency class) is rewritten in place in the vertex buffer
# instead of rebuilding the object's whole spatial group (research/81 in the
# sl-viewer repository). Included by plus/plus.cmake. Reached through the
# <plus> hooks in llvovolume.cpp (LLVOVolume::setTEColor,
# LLVolumeGeometryManager::rebuildGeom / rebuildMesh).

list(APPEND plus_SOURCE_FILES
    plus/tecolor/tecolor.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/tecolor/tecolor.h
    )
