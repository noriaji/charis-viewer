# -*- cmake -*-
# Lost attachments: kills of the agent's attachments in a teleport or a region
# crossing are ignored, and the attachments asked for again (research/146, in
# the sl-viewer repository). Included by plus/plus.cmake. Hooks: llviewermessage.cpp
# (process_kill_object, process_crossed_region), llviewerdisplay.cpp (teleport
# arrival), llvoavatarself.cpp (updateRegion), llviewermenu.cpp (the menu function).

list(APPEND plus_SOURCE_FILES
    plus/lostattach/lostattach.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/lostattach/lostattach.h
    )
