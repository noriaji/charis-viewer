# -*- cmake -*-
# Vulkan start-up: ends the viewer with a message when this PC cannot run the
# Vulkan renderer, and why (research/82-vulkan-min-spec.md, 85-gl-removal.md in
# the sl-viewer repository). Included by plus/plus.cmake. Hook:
# llfeaturemanager.cpp loadGPUClass (the same GPU named by GL or by Vulkan
# keeps the graphics settings, research/117 7.1; plus/plus-hooks.sh lists it).

list(APPEND plus_SOURCE_FILES
    plus/vkcheck/vkcheck.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/vkcheck/vkcheck.h
    )
