# -*- cmake -*-
# The volumetric clouds (PlusVolumetricClouds, research/186 in the
# sl-viewer repository). Included by plus/plus.cmake. Hooked in
# lldrawpoolwlsky.cpp (renderSkyCloudsDeferred); the shader part is
# app_settings/shaders/class1/deferred/cloudsF.glsl (VOLUMETRIC).

list(APPEND plus_SOURCE_FILES
    plus/volclouds/volclouds.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/volclouds/volclouds.h
    )
