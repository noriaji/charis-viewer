# -*- cmake -*-
# After a teleport, the frame time, VRAM and what the old region still holds on
# a timeline (tag TPLag), and the switches of the GL buffer compaction
# (research/245-tp-lag.md in the sl-viewer repository). Included by
# plus/plus.cmake. No hook in an official file.

list(APPEND plus_SOURCE_FILES
    plus/tplag/tplag.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/tplag/tplag.h
    )
