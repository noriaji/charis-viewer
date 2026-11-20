# -*- cmake -*-
# A census of the attachments of the avatars in view: those the region lists, those worn, those
# asleep in the region's object cache or in its orphan map, those nowhere (LLRHI_ATTACH_DIAG=1,
# research/355 stage 0 (5.1) in the sl-viewer repository). Included by plus/plus.cmake.
# Hook: the idle loop of llappviewer.cpp (once a frame, next to the idle census).

list(APPEND plus_SOURCE_FILES
    plus/attachdiag/attachdiag.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/attachdiag/attachdiag.h
    )
