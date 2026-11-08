# -*- cmake -*-
# Depth of field behind alpha (PlusDoFAlpha): the alpha pool's depth pass for
# depth of field writes an alpha surface's depth only where it is mostly
# opaque, so the background seen through a hair's clear parts is blurred too
# (handoff 2026-10-20-dof-alpha in the sl-viewer repository). Included by
# plus/plus.cmake. Called by the <plus> hooks in lldrawpoolalpha.cpp.

list(APPEND plus_SOURCE_FILES
    plus/dofalpha/dofalpha.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/dofalpha/dofalpha.h
    )
