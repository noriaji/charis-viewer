# -*- cmake -*-
# Looks: colour grading of the finished image with a LUT (.cube or a
# ReShade-style PNG strip), picked from the Charis menu
# (research/102-community-looks.md in the sl-viewer repository). Included by
# plus/plus.cmake. Drawn through the <plus> hook in
# LLPipeline::renderFinalize() (pipeline.cpp); the pass is recorded by the
# Vulkan backend (llvulkan/llvklook.cpp). lutfile.* has no viewer
# dependencies (tested standalone, see tools/looks in the sl-viewer
# repository). The shipped looks are in app_settings/plus/looks/.

list(APPEND plus_SOURCE_FILES
    plus/look/look.cpp
    plus/look/lookfx.cpp
    plus/look/lutfile.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/look/look.h
    plus/look/lookfx.h
    plus/look/lutfile.h
    )
