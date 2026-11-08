# -*- cmake -*-
# Destinations reload (plus/destinations): a Reload button on the Destinations floater, and a
# reload when the floater is opened again after the guide page failed to load (HTTP 4xx/5xx).
# Included by plus/plus.cmake. Hooks: llfloaterdestinations.{h,cpp} (postBuild, onOpen).

list(APPEND plus_SOURCE_FILES
    plus/destinations/destinations.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/destinations/destinations.h
    )
