# -*- cmake -*-
# The night clouds under an auto-adjusted classic sky, kept from the tone curve's crushing
# (research/176-star-field.md section 10 in the sl-viewer repository; PlusNightCloudLift).
# Included by plus/plus.cmake. cloudsF.glsl does the lift; lldrawpoolwlsky.cpp sets it.

list(APPEND plus_SOURCE_FILES
    plus/nightclouds/nightclouds.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/nightclouds/nightclouds.h
    )
