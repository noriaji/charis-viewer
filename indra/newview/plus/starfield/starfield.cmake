# -*- cmake -*-
# The night sky's stars and the Milky Way (research/176-star-field.md in the
# sl-viewer repository). Included by plus/plus.cmake. LLVOWLSky (llvowlsky.cpp)
# builds the vertex buffer from the catalogue; starsV.glsl / starsF.glsl draw it.

list(APPEND plus_SOURCE_FILES
    plus/starfield/starfield.cpp
    plus/starfield/skymilky.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/starfield/starfield.h
    plus/starfield/skymilky.h
    plus/starfield/starfieldmath.h
    )
