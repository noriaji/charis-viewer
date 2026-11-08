# -*- cmake -*-
# The parcel's weather: a [slwx ...] tag in a parcel's description, in
# Slipstream's format, becomes rain, snow and wind for everybody who
# reads it (research/229-slwx-parcel-weather.md in the sl-viewer repository).
# Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/slwx/slwx.cpp
    plus/slwx/slwxfloater.cpp
    plus/slwx/slwxparcel.cpp
    plus/slwx/slwxsync.cpp
    plus/slwx/slwxtag.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/slwx/slwx.h
    plus/slwx/slwxfloater.h
    plus/slwx/slwxparcel.h
    plus/slwx/slwxsync.h
    plus/slwx/slwxtag.h
    )
