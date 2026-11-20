# -*- cmake -*-
# Quick animation preview: double-clicking an animation in the inventory reuses one
# preview window and plays at once (research/332). Included by plus/plus.cmake.
# Hooks: LLAnimationBridgeAction::doIt (llinventorybridge.cpp), LLPreviewAnim (llpreviewanim.cpp).

list(APPEND plus_SOURCE_FILES
    plus/animquick/animquick.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/animquick/animquick.h
    )
