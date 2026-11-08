# -*- cmake -*-
# Derez: a deleted object crumbles into glowing blocks that fly up (research/257 in the sl-viewer
# repository). Included by plus/plus.cmake. Hooks: llviewermessage.cpp (process_kill_object),
# pipeline.cpp (renderGeomDeferred), llviewershadermgr.cpp (gDeferredDerezProgram: deferred/derezV.glsl,
# deferred/derezF.glsl, and deferred/derezClock.glsl, the cells' clock, in both stages).

list(APPEND plus_SOURCE_FILES
    plus/derez/derez.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/derez/derez.h
    )
