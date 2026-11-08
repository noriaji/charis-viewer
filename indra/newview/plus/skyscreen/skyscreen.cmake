# -*- cmake -*-
# How the scene takes the sky's colour to the screen, for the sky's shaders that undo it
# (research/176-star-field.md section 9 in the sl-viewer repository): the star field
# (starsF.glsl) and the night clouds (cloudsF.glsl). Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/skyscreen/skyscreen.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/skyscreen/skyscreen.h
    )
