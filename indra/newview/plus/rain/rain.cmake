# -*- cmake -*-
# Rain: falling streaks and splashes, and the overhead depth map that keeps
# them and the wet look off what lies under a roof (research/120-rain-idea.md
# in the sl-viewer repository, stages 2 and 3). Included by plus/plus.cmake.
# The map is drawn through the <plus> hook in display() (llviewerdisplay.cpp)
# with the official shadow caster path; the rain through the <plus> hook in
# LLPipeline::renderFinalize() (pipeline.cpp), recorded by the Vulkan backend
# (llvulkan/llvkrain.cpp).

list(APPEND plus_SOURCE_FILES
    plus/rain/overhead.cpp
    plus/rain/rain.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/rain/overhead.h
    plus/rain/rain.h
    )
