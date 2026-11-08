# -*- cmake -*-
# Test scene: a made-up load of viewer-only objects above the avatar for repeatable frame-time measurements
# (research/171-local-test-scene.md in the sl-viewer repository; development only, PlusTestScene).
# Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/testscene/testscene.cpp
    plus/testscene/testscenedef.cpp
    plus/testscene/testscenetools.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/testscene/testscene.h
    plus/testscene/testscenedef.h
    plus/testscene/testscenetools.h
    )
