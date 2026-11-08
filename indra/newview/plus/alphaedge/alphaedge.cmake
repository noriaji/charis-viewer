# -*- cmake -*-
# The colour of a texture's transparent texels pushed out from the opaque ones when it is decoded
# (PlusAlphaEdgeFix, research/alpha-mask-edge-2026-10-26.md (d) in the sl-viewer repository): no
# dark rim at the edges of alpha masks and blended textures (the moon). Included by
# plus/plus.cmake. Hooks: llviewertexture.cpp (LLViewerTexture::addFace), lltexturefetch.cpp
# (DecodeResponder::completed), llvovolume.cpp (setTEMaterialParams, the PBR faces in rebuildGeom).

list(APPEND plus_SOURCE_FILES
    plus/alphaedge/alphaedge.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/alphaedge/alphaedge.h
    )
