# -*- cmake -*-
# Fine adjustment of the avatar's facing and position: Ctrl+Left/Right turn by a
# small fixed angle, Ctrl+Shift+arrows nudge the position, and the same buttons
# in the Move floater. Included by plus/plus.cmake.
# Hooks: llviewerinput.cpp (keyboard actions), llappviewer.cpp (default keys),
# llmoveview.cpp (floater buttons), floater_moveview.xml (the panel).

list(APPEND plus_SOURCE_FILES
    plus/nudge/nudge.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/nudge/nudge.h
    )
