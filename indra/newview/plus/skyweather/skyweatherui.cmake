# -*- cmake -*-
# The weather tab of the sky editors (research/371-weather-tab.md, branch C).
# Kept apart from skyweather.cmake (the reading side) so the two branches do
# not touch the same file. Included by plus/plus.cmake (it globs */*.cmake).

list(APPEND plus_SOURCE_FILES
    plus/skyweather/skyweatherpanel.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/skyweather/skyweatherpanel.h
    )
