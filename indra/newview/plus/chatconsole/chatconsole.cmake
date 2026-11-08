# -*- cmake -*-
# Chat console (plus/chatconsole): nearby chat lines at the bottom left of the screen while the chat window
# is not open, as Firestorm's console. Included by plus/plus.cmake. One official hook:
# plus::chatconsole::replacesToasts() in LLFloaterIMNearbyChatHandler::processChat() (no toast while the
# console shows the line). The view goes into the window's root view from the feature's onUIReady, the lines come from the "LLChat" event
# pump, its menu item is in menu_plus.xml and its settings (PlusChatConsole*) in settings_plus.xml.

list(APPEND plus_SOURCE_FILES
    plus/chatconsole/chatconsole.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/chatconsole/chatconsole.h
    )
