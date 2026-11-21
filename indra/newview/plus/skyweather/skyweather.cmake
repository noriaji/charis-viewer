# -*- cmake -*-
# The weather a sky or day cycle carries (its "weather" key, LLSkyWeather in
# llinventory): read as rain, snow and wind through plus/slwx's Effective
# (research/371-weather-tab.md in the sl-viewer repository, branch B).
# Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/skyweather/skyweather.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/skyweather/skyweather.h
    plus/skyweather/skyweathercore.h
    )
