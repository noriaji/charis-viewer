# -*- cmake -*-
# Textures on screen first: view tiers, fetch and creation order, the floor
# for on-screen textures with no image yet (research/285-vram-pressure.md in
# the sl-viewer repository, tex-view-priority). Included by plus/plus.cmake.
# Hooks: llface.cpp (the view cone), llviewertexturelist.cpp (the face scan,
# the creation queue, the fetch pass's counters), llviewertexture.cpp (the
# fetch priority and hold, the switches once a frame).

list(APPEND plus_SOURCE_FILES
    plus/texprio/texprio.cpp
    plus/texprio/fetchflow.cpp
    plus/texprio/fetchretry.cpp
    plus/texcache/posfile.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/texprio/texprio.h
    plus/texprio/fetchflow.h
    plus/texprio/fetchretry.h
    plus/texcache/posfile.h
    )
