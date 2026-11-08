# -*- cmake -*-
# The Charis features on the official quality levels (featuretable.txt), the
# one-time move from the retired Charis presets, and the snapshot boost
# (research/183-preset-mapping-to-official-7.md in the sl-viewer repository).
# Included by plus/plus.cmake. The boost is held by the <plus> hook in
# LLViewerWindow's snapshot paths (llviewerwindow.cpp).

list(APPEND plus_SOURCE_FILES
    plus/presets/presets.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/presets/presets.h
    )
