# -*- cmake -*-
# Quick Preferences: a small window with the settings the user picks, and the
# environment presets (from Firestorm's quickprefs; see SOURCE.md). Included by
# plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/quickprefs/quickprefs.cpp
    plus/quickprefs/quickprefsfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/quickprefs/quickprefsfloater.h
    )
