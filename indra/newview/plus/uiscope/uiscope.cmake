# -*- cmake -*-
# The common part of hiding the viewer's UI for a while (the photo screen, the camera path's and the camera shots' hidden UI; research/365, 367
# in the sl-viewer repository). Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/uiscope/uiscope.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/uiscope/uiscope.h
    )
