# -*- cmake -*-
# The pose bridge (research/336): an attachment with a script that carries Aurora-compatible pose-consent
# messages; ported in part from Firestorm's LSL bridge (see ORIGIN.md). Included by plus/plus.cmake.
# The chat hook is in process_chat_from_simulator() (llviewermessage.cpp, <plus>); the script is
# app_settings/plus/lslbridge/charis_bridge.lsl.

list(APPEND plus_SOURCE_FILES
    plus/lslbridge/lslbridge.cpp
    plus/lslbridge/lslbridgeproto.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/lslbridge/lslbridge.h
    plus/lslbridge/lslbridgehooks.h
    plus/lslbridge/lslbridgeproto.h
    )

# lslbridgeproto_test.cpp is a stand-alone test (cl /EHsc /std:c++17 lslbridgeproto_test.cpp lslbridgeproto.cpp),
# not built here.
