# -*- cmake -*-
# Avatars in reflections (research/127-avatar-planar-reflection.md in the
# sl-viewer repository): stage 0, avatars always in mirrors; stage 1, your
# avatar mirrored in the floor or water under it. Included by plus/plus.cmake.
# Hooks: llheroprobemanager.cpp (renderProbes), llviewerdisplay.cpp (display,
# after the mirrors), llviewershadermgr.cpp (loadBasicShaders, the define),
# pipeline.cpp (bindReflectionProbes) and class3/deferred/reflectionProbeF.glsl.
# The water mirror (watermirror.*, research/139) rides on the same hooks, and
# has one of its own in llsettingsvo.cpp (LLSettingsVOWater::applySpecial).

list(APPEND plus_SOURCE_FILES
    plus/avatarrefl/avatarrefl.cpp
    plus/avatarrefl/watermirror.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/avatarrefl/avatarrefl.h
    plus/avatarrefl/watermirror.h
    )
