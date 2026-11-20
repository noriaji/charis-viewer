# -*- cmake -*-
# Chat bar (plus/chatbar, research/334): Firestorm's chat bar at the bottom left, a "Nearby Chat" button and a
# one-line nearby chat field beside the bottom toolbar. Included by plus/plus.cmake. The layout is in the official
# panel_toolbar_view.xml (marked); the hooks: plus::chatbar::syncBottomPanel() in LLToolBarView::draw(), and
# plus::chatbar::bottomToolbarCenterX() where buttons are centred over the bottom toolbar (llmoveview.cpp,
# llfloaterworldmap.cpp). Typing and sending are LLFloaterIMNearbyChat's (onChatEntryKeystroke / sendChatFromEntry).
# Its setting (PlusShowChatBar) is in settings_plus.xml, its switch in Preferences > Charis > Display.

list(APPEND plus_SOURCE_FILES
    plus/chatbar/chatbar.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/chatbar/chatbar.h
    plus/chatbar/chatbarlayout.h
    )
