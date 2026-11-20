# -*- cmake -*-
# What the texture list's pass (pre-gbuffer images list) takes, and why: stages,
# budget cut-offs, the rigged faces' joints (LLRHI_IMAGES_CENSUS=1, research/348
# branch 0 in the sl-viewer repository). Included by plus/plus.cmake.
# Hooks: LLViewerTextureList::updateImages, updateImagesFetchTextures,
# updateImagesUrgent, LLFace::calcPixelArea (the rigged walk).

list(APPEND plus_SOURCE_FILES
    plus/imagescensus/imagescensus.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/imagescensus/imagescensus.h
    )
