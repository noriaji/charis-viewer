# -*- cmake -*-
# Smooth texture scrolls worked out where they are drawn: the face matrices
# hold the scroll's description (indra/llrhi/llrhitexanim.h) and
# LLViewerTextureAnim::updateClass leaves the animation out (research/194 11
# in the sl-viewer repository). Included by plus/plus.cmake. Hooks:
# LLViewerTextureAnim::updateClass, LLVOVolume::animateTextures,
# processUpdateMessage and updateGeometry, llface.cpp (the small faces' bake,
# surfaceToTexture); the backend's matrix writes (llvkgl_program.cpp,
# llvkgl_descriptors.cpp, llvknative.cpp).

list(APPEND plus_SOURCE_FILES
    plus/texanim/texanim.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/texanim/texanim.h
    )
