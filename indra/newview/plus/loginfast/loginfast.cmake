# -*- cmake -*-
# Fast login and teleport arrival, and their timelines in the log
# (research/212, research/241). Included by plus/plus.cmake. Hooks: llstartup.cpp
# (setStartupState, STATE_PRECACHE, STATE_WEARABLES_WAIT), llprogressview.cpp
# (the fade into the world), llviewerdisplay.cpp (update_tp_display),
# llviewermessage.cpp (the far range after an arrival).

list(APPEND plus_SOURCE_FILES
    plus/loginfast/loginfast.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/loginfast/loginfast.h
    )
