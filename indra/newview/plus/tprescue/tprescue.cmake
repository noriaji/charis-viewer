# -*- cmake -*-
# Teleport rescue: the viewer does not disconnect itself when the old region
# goes before the new one has answered, and asks the new one again
# (research/43-relog-in-process.md, stage R1, in the sl-viewer repository).
# Included by plus/plus.cmake. Hooks: llviewermessage.cpp (TeleportFinish),
# llworld.cpp (removeRegion), llappviewer.cpp (the circuit check),
# lleventpoll.cpp (the stalled event poll).

list(APPEND plus_SOURCE_FILES
    plus/tprescue/tprescue.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/tprescue/tprescue.h
    )
