# -*- cmake -*-
# FPS in a corner of the screen (plus/fpscorner): "87 FPS" on a dim box, the median of the FPS stat
# over the last 50 loops, read every half second. Included by plus/plus.cmake. No official file is changed: the view
# goes into the window's root view from the feature's onUIReady, its switches are in panel_preferences_charis.xml
# and its settings (PlusFpsCorner*) in settings_plus.xml. fpscorner_test.cpp is a stand-alone test of
# fpscornermath.h, not built here.

list(APPEND plus_SOURCE_FILES
    plus/fpscorner/fpscorner.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/fpscorner/fpscornermath.h
    )
