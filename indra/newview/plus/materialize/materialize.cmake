# -*- cmake -*-
# The materialize band (research/256-materialize.md in the sl-viewer
# repository): the settings, colours and count of the band that sweeps a
# surface from its loading wireframe to the real thing when its texture comes.
# Drawn by GrayWire (llgraywire.cpp); included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/materialize/materialize.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/materialize/materialize.h
    )
