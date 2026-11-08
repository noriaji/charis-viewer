# -*- cmake -*-
# The real night sky (research/177-real-sky-stars.md in the sl-viewer repository): the Yale
# Bright Star Catalogue where it stands for a place and a time, and the constellation lines.
# Included by plus/plus.cmake. LLVOWLSky fills the star field from it; lldrawpoolwlsky.cpp turns
# the sky (realstars.h frame()). realsky_test.cpp is the stand-alone test (tools/selftest).

list(APPEND plus_SOURCE_FILES
    plus/realstars/realstars.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/realstars/realstars.h
    plus/realstars/realskymath.h
    plus/realstars/realskyplaces.h
    plus/realstars/realskytime.h
    plus/realstars/realstarcat.h
    plus/realstars/realstarart.h
    )
