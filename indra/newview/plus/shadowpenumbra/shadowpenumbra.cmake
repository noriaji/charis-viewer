# -*- cmake -*-
# A penumbra that grows with the distance from the caster in the sun's shadows
# (PlusShadowPenumbra, research/187 in the sl-viewer repository). Included by
# plus/plus.cmake. Hooked in pipeline.cpp (the sun light pass of
# renderDeferredLighting); the shader part is in
# app_settings/shaders/class1/deferred/shadowUtil.glsl.

list(APPEND plus_SOURCE_FILES
    plus/shadowpenumbra/shadowpenumbra.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shadowpenumbra/shadowpenumbra.h
    )
