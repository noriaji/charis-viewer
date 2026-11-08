# -*- cmake -*-
# Radar (plus/radar, from Firestorm's FSRadar): the avatars around you with their distance,
# reports in the nearby chat when one comes into or goes out of range (research/140, 142).
# Included by plus/plus.cmake. The window is registered from the feature's init and its menu
# item is in menu_plus.xml; no official file is changed.

list(APPEND plus_SOURCE_FILES
    plus/radar/radar.cpp
    plus/radar/radarfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/radar/radar.h
    )
