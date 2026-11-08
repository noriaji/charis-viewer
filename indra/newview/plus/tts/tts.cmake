# -*- cmake -*-
# Chat read-aloud (plus/tts; research/ the chat read-aloud note): speaks the chat that arrives with the
# Windows speech synthesizer, a voice for each speaker. Included by plus/plus.cmake. The window is
# registered from the feature's init and its menu item is in menu_plus.xml; the official hooks
# (llfloaterimnearbychat.cpp, llimview.cpp, llviewermessage.cpp, menu_avatar_other.xml) are marked <plus>.

list(APPEND plus_SOURCE_FILES
    plus/tts/tts.cpp
    plus/tts/ttsfloater.cpp
    plus/tts/ttsengine_winrt.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/tts/tts.h
    plus/tts/ttsengine.h
    )

# The engine file includes C++/WinRT (Windows SDK) and nothing of the viewer: no precompiled header.
set_source_files_properties(plus/tts/ttsengine_winrt.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
