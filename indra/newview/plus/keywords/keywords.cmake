# -*- cmake -*-
# Chat keyword alerts (plus/keywords, from Firestorm's FSKeywords; research/142): a line of chat that
# holds a listed word gets a color, a sound and/or a notice. Included by plus/plus.cmake. The window
# is registered from the feature's init and its menu item is in menu_plus.xml; the official hooks
# (llviewermessage.cpp, llimview.cpp, llviewerchat.cpp) are marked <plus>.

list(APPEND plus_SOURCE_FILES
    plus/keywords/keywords.cpp
    plus/keywords/keywordsfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/keywords/keywords.h
    )
