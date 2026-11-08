# -*- cmake -*-
# Skin subsurface scattering (research/100-skin-sss.md in the sl-viewer
# repository). Stage 1: the skin mark in the main view's stencil and a view of
# it. Included by plus/plus.cmake. Hooks: pipeline.cpp (screen buffers, the
# G-buffer pass, occlusion, the lighting), lldrawpool.cpp (pushDirectBatch),
# lldrawpoolavatar.cpp (the system body), llvovolume.cpp (genDrawInfo); the
# passes are recorded by the Vulkan backend (llvulkan/llvkskinsss.cpp).

list(APPEND plus_SOURCE_FILES
    plus/skinsss/skinsss.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/skinsss/skinsss.h
    )
