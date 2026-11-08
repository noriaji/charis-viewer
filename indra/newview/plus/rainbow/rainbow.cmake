# -*- cmake -*-
# The rainbow worked out per pixel in the sky's shaders (research/255-rainbows.md in the sl-viewer
# repository, stage 1: PlusRainbow, PlusRainbowKind, PlusRainbowStrength). Included by plus/plus.cmake.
# skyRainbowF.glsl works the bow out; skyF.glsl and cloudsF.glsl add it (llviewershadermgr.cpp links it
# into the sky's and the clouds' programs); lldrawpoolwlsky.cpp hands them these uniforms.

list(APPEND plus_SOURCE_FILES
    plus/rainbow/rainbow.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/rainbow/rainbow.h
    )
