# -*- cmake -*-
# Media plugins nobody uses are shut down early (plus/mediareap): in-world media not drawn and far
# away, and UI browsers whose floater is closed, get PRIORITY_UNLOADED after a wait (shorter after a
# teleport, or while more plugins run than PluginInstancesTotal). PlusMediaReap switches it. Included
# by plus/plus.cmake. Hook: llviewermedia.cpp (LLViewerMedia::updateMedia(): a shut-down impl stays
# unloaded until it is wanted again).

list(APPEND plus_SOURCE_FILES
    plus/mediareap/mediareap.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/mediareap/mediareap.h
    )
